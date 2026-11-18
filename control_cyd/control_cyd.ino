// Soccer remote for the Cheap Yellow Display (ESP32-2432S024R, resistive touch).
// Same ESP-NOW protocol and safety logic as control_tx; the physical joystick,
// button, RGB LED and 16x2 LCD are replaced by the touch screen and onboard LED.
//
// Pin map verified on the real board with tools/cyd_probe (resistive XPT2046 on the TFT bus).
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "soccer_protocol.h"
#include "esp_now_compat.h"

#if __has_include("team_config.h")
#include "team_config.h"
#else
#include "team_config.example.h"
#warning "Compilando con configuracion de ejemplo; el control no se habilitara hasta crear control_cyd/team_config.h."
#endif

// Normally selected in team_config.h; the default keeps example builds working.
#ifndef PAIR_ID
#define PAIR_ID 1
#endif

static_assert(PAIR_ID >= 1 && PAIR_ID <= TEAM_PAIR_COUNT,
              "PAIR_ID debe estar entre 1 y TEAM_PAIR_COUNT");

// ---- Board pins (ESP32-2432S024R) ----
// Onboard RGB LED is active low.
constexpr uint8_t PIN_LED_R = 4;
constexpr uint8_t PIN_LED_G = 16;
constexpr uint8_t PIN_LED_B = 17;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = false;     // Some CYD lots need true: verify colors.
      cfg.rgb_order = false;  // Some CYD lots need true: verify colors.
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;  // the touch controller uses the same bus
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      // Verified with tools/cyd_probe: the XPT2046 shares the TFT SPI bus
      // (CLK 14, MOSI 13, MISO 12), with its own CS 33 and pen IRQ 36.
      auto cfg = touch_.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3700;
      cfg.pin_int = 36;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_cs = 33;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};

// ---- UI layout (landscape 320x240) ----
constexpr int16_t SCREEN_W = 320;
constexpr int16_t SCREEN_H = 240;
constexpr int16_t HEADER_H = 30;
constexpr int16_t JOY_CX = 95;
constexpr int16_t JOY_CY = 140;
constexpr int16_t JOY_RADIUS = 75;   // Pixels for full deflection (+-1000).
constexpr int16_t JOY_GRAB_MARGIN = 10;
constexpr int16_t KNOB_RADIUS = 22;
constexpr int16_t STATUS_X = 185, STATUS_Y = 42, STATUS_W = 128, STATUS_H = 70;
constexpr int16_t BUTTON_X = 185, BUTTON_Y = 125, BUTTON_W = 128, BUTTON_H = 100;
constexpr uint32_t UI_MIN_FRAME_MS = 33;  // ~30 fps cap for joystick redraw.
constexpr uint8_t TOUCH_CONFIRM_SAMPLES = 2;
constexpr uint32_t RECALIBRATE_WINDOW_MS = 1500;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SEND_INTERVAL_US = 20000;  // 50 Hz
constexpr uint32_t SEQUENCE_BLOCK_SIZE = 10000;
constexpr int16_t JOYSTICK_DEADZONE = 50;  // On the normalized -1000..1000 scale

constexpr uint32_t STATUS_TIMEOUT_MS = 300;
constexpr uint32_t BUTTON_STABLE_MS = 30;
constexpr uint8_t SENT_HISTORY_SIZE = 32;

Preferences preferences;    // soccer_tx: anti-replay counter (same as control_tx)
Preferences uiPreferences;  // cyd_ui: touch calibration only
const TeamPairConfig &config = teamPair(PAIR_ID);
LGFX lcd;

uint32_t nextSequence = 1;
uint32_t reservedUntil = 1;
uint32_t nextSendAtUs = 0;
bool radioReady = false;
bool uiReady = false;
uint8_t lastDisplayedStatus = 255;

// Loop-only input state.
enum class Zone : uint8_t { NONE, JOYSTICK, BUTTON };
struct InputState {
  int16_t steering;
  int16_t throttle;
  bool button;
};
InputState input = {};
Zone engaged = Zone::NONE;
Zone pendingZone = Zone::NONE;
uint8_t pendingCount = 0;
int16_t knobX = JOY_CX, knobY = JOY_CY;
bool buttonDrawn = false;
uint32_t lastUiFrameMs = 0;

// All callback/loop shared data below is protected by statusMux.
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t consecutiveSendFailures = 0;
bool hasRobotStatus = false;
StatusMsg latestStatus = {};
uint32_t lastStatusAtMs = 0;
struct SentPacket {
  uint32_t seq;
  uint32_t sentAtMs;
  bool valid;
};
SentPacket sentHistory[SENT_HISTORY_SIZE] = {};
uint8_t nextHistorySlot = 0;  // loop only

void printMac(const uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Onboard LED is active low.
void setStatusColor(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}

// ---- Drawing ----
void drawHeader() {
  lcd.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_DARKGREY);
  lcd.setTextColor(TFT_WHITE, TFT_DARKGREY);
  lcd.setTextDatum(lgfx::middle_left);
  lcd.setFont(&fonts::Font4);
  char title[24];
  snprintf(title, sizeof(title), "Control PAIR %u", PAIR_ID);
  lcd.drawString(title, 8, HEADER_H / 2);
}

void drawStatusPanel(uint16_t color, const char *text) {
  lcd.fillRoundRect(STATUS_X, STATUS_Y, STATUS_W, STATUS_H, 8, color);
  lcd.setTextColor(color == TFT_YELLOW ? TFT_BLACK : TFT_WHITE, color);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.setFont(&fonts::Font2);
  lcd.drawString(text, STATUS_X + STATUS_W / 2, STATUS_Y + STATUS_H / 2);
}

void drawActionButton(bool pressed) {
  const uint16_t color = pressed ? TFT_RED : TFT_MAROON;
  lcd.fillRoundRect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, 10, color);
  lcd.setTextColor(TFT_WHITE, color);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.setFont(&fonts::Font2);
  lcd.drawString("ARMAR / FRENO", BUTTON_X + BUTTON_W / 2, BUTTON_Y + BUTTON_H / 2);
  buttonDrawn = pressed;
}

// Repaints the joystick base and knob, restricted to the current clip rectangle.
void paintJoystick() {
  lcd.fillRect(JOY_CX - JOY_RADIUS - KNOB_RADIUS, JOY_CY - JOY_RADIUS - KNOB_RADIUS,
               2 * (JOY_RADIUS + KNOB_RADIUS) + 1, 2 * (JOY_RADIUS + KNOB_RADIUS) + 1,
               TFT_BLACK);
  lcd.fillCircle(JOY_CX, JOY_CY, JOY_RADIUS + KNOB_RADIUS, 0x2104);  // dark gray
  lcd.drawCircle(JOY_CX, JOY_CY, JOY_RADIUS + KNOB_RADIUS, TFT_LIGHTGREY);
  lcd.drawFastHLine(JOY_CX - JOY_RADIUS, JOY_CY, 2 * JOY_RADIUS, TFT_DARKGREY);
  lcd.drawFastVLine(JOY_CX, JOY_CY - JOY_RADIUS, 2 * JOY_RADIUS, TFT_DARKGREY);
  lcd.fillCircle(knobX, knobY, KNOB_RADIUS, TFT_CYAN);
}

void redrawJoystickRegion(int16_t cx, int16_t cy) {
  lcd.setClipRect(cx - KNOB_RADIUS - 1, cy - KNOB_RADIUS - 1,
                  2 * KNOB_RADIUS + 3, 2 * KNOB_RADIUS + 3);
  paintJoystick();
  lcd.clearClipRect();
}

void drawStaticUi() {
  lcd.fillScreen(TFT_BLACK);
  drawHeader();
  paintJoystick();
  drawActionButton(false);
}

void showMessage(const char *line1, const char *line2 = nullptr) {
  if (!uiReady) {
    return;
  }
  lcd.fillScreen(TFT_BLACK);
  drawHeader();
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.setFont(&fonts::Font4);
  lcd.drawString(line1, SCREEN_W / 2, line2 ? 105 : 130);
  if (line2) {
    lcd.setFont(&fonts::Font2);
    lcd.drawString(line2, SCREEN_W / 2, 150);
  }
}

[[noreturn]] void fatalError(const char *message, esp_err_t error = ESP_OK) {
  Serial.print("ERROR: ");
  Serial.print(message);
  if (error != ESP_OK) {
    Serial.print(" (");
    Serial.print(esp_err_to_name(error));
    Serial.print(")");
  }
  Serial.println();

  if (uiReady) {
    lcd.fillScreen(TFT_RED);
    lcd.setTextColor(TFT_WHITE, TFT_RED);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.setFont(&fonts::Font4);
    lcd.drawString("ERROR FATAL", SCREEN_W / 2, SCREEN_H / 2);
  }

  while (true) {
    setStatusColor(true, false, false);
    delay(150);
    setStatusColor(false, false, false);
    delay(150);
  }
}

// ---- Touch input ----
bool readTouch(int32_t &x, int32_t &y) {
  return lcd.getTouch(&x, &y);
}

void waitForTouchState(bool touched) {
  uint32_t stableSince = millis();
  int32_t x, y;
  while (static_cast<uint32_t>(millis() - stableSince) < BUTTON_STABLE_MS) {
    if (readTouch(x, y) != touched) {
      stableSince = millis();
    }
    delay(1);
  }
}

bool loadTouchCalibration(uint16_t (&data)[8]) {
  if (!uiPreferences.begin("cyd_ui", true)) {
    return false;
  }
  const size_t read = uiPreferences.getBytes("touch", data, sizeof(data));
  uiPreferences.end();
  return read == sizeof(data);
}

void saveTouchCalibration(const uint16_t (&data)[8]) {
  if (uiPreferences.begin("cyd_ui", false)) {
    uiPreferences.putBytes("touch", data, sizeof(data));
    uiPreferences.end();
  } else {
    Serial.println("Aviso: no se pudo guardar la calibracion tactil en NVS");
  }
}

// Radio stays off during touch calibration and the start tap.
void prepareTouch() {
  uint16_t calibration[8] = {};
  bool haveCalibration = loadTouchCalibration(calibration);

  if (haveCalibration) {
    showMessage("Mantener toca", "para recalibrar");
    const uint32_t startedAt = millis();
    int32_t x, y;
    while (static_cast<uint32_t>(millis() - startedAt) < RECALIBRATE_WINDOW_MS) {
      if (readTouch(x, y)) {
        haveCalibration = false;
        break;
      }
      delay(10);
    }
  }

  if (!haveCalibration) {
    Serial.println("Calibrando pantalla tactil: tocar las marcas");
    lcd.calibrateTouch(calibration, TFT_WHITE, TFT_BLACK, 15);
    saveTouchCalibration(calibration);
  }
  lcd.setTouchCalibrate(calibration);

  // This tap only starts the radio; it cannot count toward arming.
  showMessage("Toca para iniciar");
  Serial.println("Toca la pantalla para iniciar el control");
  waitForTouchState(false);
  waitForTouchState(true);
  showMessage("Suelta la pantalla");
  waitForTouchState(false);
}

Zone zoneAt(int32_t x, int32_t y) {
  const int32_t dx = x - JOY_CX;
  const int32_t dy = y - JOY_CY;
  const int32_t grab = JOY_RADIUS + JOY_GRAB_MARGIN;
  if (dx * dx + dy * dy <= grab * grab) {
    return Zone::JOYSTICK;
  }
  if (x >= BUTTON_X && x < BUTTON_X + BUTTON_W &&
      y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H) {
    return Zone::BUTTON;
  }
  return Zone::NONE;
}

int16_t applyDeadzone(int32_t value) {
  value = constrain(value, -1000, 1000);
  return abs(value) < JOYSTICK_DEADZONE ? 0 : static_cast<int16_t>(value);
}

// No contact always means neutral. A touch must start inside a zone and be
// confirmed on consecutive samples before it is used.
void sampleInput() {
  int32_t x = 0, y = 0;
  if (!readTouch(x, y)) {
    engaged = Zone::NONE;
    pendingZone = Zone::NONE;
    pendingCount = 0;
    input = {0, 0, false};
    return;
  }

  const Zone zone = zoneAt(x, y);
  if (engaged == Zone::NONE) {
    if (zone != Zone::NONE && zone == pendingZone) {
      if (++pendingCount >= TOUCH_CONFIRM_SAMPLES) {
        engaged = zone;
      }
    } else {
      pendingZone = zone;
      pendingCount = zone != Zone::NONE ? 1 : 0;
    }
  }

  if (engaged == Zone::JOYSTICK) {
    // Clamp to the circle, then scale to +-1000. Up is forward.
    int32_t dx = x - JOY_CX;
    int32_t dy = y - JOY_CY;
    const int32_t distSq = dx * dx + dy * dy;
    if (distSq > static_cast<int32_t>(JOY_RADIUS) * JOY_RADIUS) {
      const float scale = static_cast<float>(JOY_RADIUS) / sqrtf(static_cast<float>(distSq));
      dx = static_cast<int32_t>(dx * scale);
      dy = static_cast<int32_t>(dy * scale);
    }
    input.steering = applyDeadzone(dx * 1000 / JOY_RADIUS);
    input.throttle = applyDeadzone(-dy * 1000 / JOY_RADIUS);
    input.button = false;
    knobX = JOY_CX + dx;
    knobY = JOY_CY + dy;
  } else if (engaged == Zone::BUTTON) {
    input = {0, 0, zone == Zone::BUTTON};
  } else {
    input = {0, 0, false};
  }
}

// ---- Sequence reservation (unchanged from control_tx) ----
void reserveSequenceBlock() {
  nextSequence = preferences.getULong("next_seq", 1);
  reservedUntil = nextSequence + SEQUENCE_BLOCK_SIZE;
  // Unsigned wrap is intentional; the receiver uses modular comparison.
  if (preferences.putULong("next_seq", reservedUntil) == 0) {
    fatalError("No se pudo reservar el contador anti-replay en NVS");
  }
}

void advanceSequenceReservation() {
  ++nextSequence;
  if (nextSequence == reservedUntil) {
    reservedUntil += SEQUENCE_BLOCK_SIZE;
    if (preferences.putULong("next_seq", reservedUntil) == 0) {
      fatalError("No se pudo actualizar el contador anti-replay en NVS");
    }
  }
}

// Radio delivery is diagnostic only; application status confirms the robot.
void onDataSent(const SoccerSendInfo *info, esp_now_send_status_t status) {
  (void)info;
  portENTER_CRITICAL(&statusMux);
  if (status == ESP_NOW_SEND_SUCCESS) {
    consecutiveSendFailures = 0;
  } else if (consecutiveSendFailures < 255) {
    ++consecutiveSendFailures;
  }
  portEXIT_CRITICAL(&statusMux);
}

void onRobotStatus(const esp_now_recv_info_t *info,
                   const uint8_t *data, int length) {
  if (info == nullptr || data == nullptr || length != sizeof(StatusMsg) ||
      memcmp(info->src_addr, config.robotMac, sizeof(config.robotMac)) != 0 ||
      memcmp(info->des_addr, config.controlMac, sizeof(config.controlMac)) != 0) {
    return;
  }
  StatusMsg status = {};
  memcpy(&status, data, sizeof(status));
  if (status.magic != SOCCER_STATUS_MAGIC ||
      status.version != SOCCER_PROTOCOL_VERSION || status.pairId != PAIR_ID ||
      status.reserved != 0 ||
      (status.state != RobotState::WAITING && status.state != RobotState::DISARMED &&
       status.state != RobotState::ARMED) || status.crc16 != soccerStatusCrc(status)) {
    return;
  }
  portENTER_CRITICAL(&statusMux);
  const uint32_t now = millis();
  bool recentCommand = false;
  for (uint8_t i = 0; i < SENT_HISTORY_SIZE; ++i) {
    if (sentHistory[i].valid && sentHistory[i].seq == status.acceptedSeq &&
        static_cast<uint32_t>(now - sentHistory[i].sentAtMs) <= STATUS_TIMEOUT_MS) {
      recentCommand = true;
      break;
    }
  }
  if (recentCommand && (!hasRobotStatus ||
      soccerIsNewerSequence(status.acceptedSeq, latestStatus.acceptedSeq))) {
    latestStatus = status;
    lastStatusAtMs = now;
    hasRobotStatus = true;
  }
  portEXIT_CRITICAL(&statusMux);
}

void initializeRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);

  esp_err_t error = esp_wifi_set_ps(WIFI_PS_NONE);
  if (error != ESP_OK) {
    fatalError("No se pudo desactivar el ahorro de energia Wi-Fi", error);
  }
  error = esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (error != ESP_OK) {
    fatalError("No se pudo fijar el canal Wi-Fi", error);
  }

  uint8_t localMac[6] = {};
  error = esp_wifi_get_mac(WIFI_IF_STA, localMac);
  if (error != ESP_OK) {
    fatalError("No se pudo leer la MAC local", error);
  }
  Serial.print("MAC del control: ");
  printMac(localMac);
  Serial.println();
  if (memcmp(localMac, config.controlMac, sizeof(localMac)) != 0) {
    Serial.print("MAC esperada para este PAIR_ID: ");
    printMac(config.controlMac);
    Serial.println();
    fatalError("PAIR_ID o team_config.h no corresponde a este control");
  }

  error = esp_now_init();
  if (error != ESP_OK) {
    fatalError("No se pudo iniciar ESP-NOW", error);
  }
  error = esp_now_set_pmk(config.pmk);
  if (error != ESP_OK) {
    fatalError("No se pudo configurar la PMK", error);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, config.robotMac, sizeof(peer.peer_addr));
  memcpy(peer.lmk, config.lmk, sizeof(peer.lmk));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;
  error = esp_now_add_peer(&peer);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el robot como peer cifrado", error);
  }

  error = esp_now_register_send_cb(onDataSent);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el callback de envio", error);
  }

  error = esp_now_register_recv_cb(onRobotStatus);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el receptor de estado", error);
  }

  radioReady = true;
  Serial.printf("Control del par %u listo en canal %u. Robot: ", PAIR_ID, WIFI_CHANNEL);
  printMac(config.robotMac);
  Serial.println();
}

void sendControlPacket() {
  RemoteMsg message = {};
  message.magic = SOCCER_COMMAND_MAGIC;
  message.version = SOCCER_PROTOCOL_VERSION;
  message.pairId = PAIR_ID;
  message.flags = input.button ? SOCCER_FLAG_JOYSTICK_BUTTON : 0;
  message.seq = nextSequence;
  message.steering = input.steering;
  message.throttle = input.throttle;
  message.crc16 = soccerMessageCrc(message);

  const uint8_t slot = nextHistorySlot;
  nextHistorySlot = (nextHistorySlot + 1) % SENT_HISTORY_SIZE;
  portENTER_CRITICAL(&statusMux);
  sentHistory[slot] = {message.seq, millis(), true};
  portEXIT_CRITICAL(&statusMux);

  const esp_err_t error = esp_now_send(
      config.robotMac, reinterpret_cast<const uint8_t *>(&message), sizeof(message));
  if (error != ESP_OK) {
    Serial.printf("No se pudo encolar paquete %lu: %s\n",
                  static_cast<unsigned long>(message.seq), esp_err_to_name(error));
    portENTER_CRITICAL(&statusMux);
    sentHistory[slot].valid = false;
    if (consecutiveSendFailures < 255) {
      ++consecutiveSendFailures;
    }
    portEXIT_CRITICAL(&statusMux);
  }
  advanceSequenceReservation();
}

void updateStatusIndicators() {
  portENTER_CRITICAL(&statusMux);
  const bool confirmed = hasRobotStatus;
  const RobotState state = latestStatus.state;
  const uint32_t statusAt = lastStatusAtMs;
  const uint8_t failures = consecutiveSendFailures;
  portEXIT_CRITICAL(&statusMux);

  const uint32_t now = millis();
  const bool fresh = confirmed && static_cast<uint32_t>(now - statusAt) <= STATUS_TIMEOUT_MS;
  const uint8_t display = !confirmed ? 0 : !fresh ? 1 :
                          state == RobotState::ARMED ? 3 : 2;
  const char *labels[] = {"Esperando robot", "Sin senal", "Desarmado", "Armado"};
  const uint16_t colors[] = {TFT_BLUE, TFT_RED, TFT_YELLOW, TFT_DARKGREEN};
  // Green while the user is actually sending an action (joystick off-center or
  // button held) over a link the robot is confirming; otherwise the state color.
  // The on-screen panel always shows the true state.
  const bool sendingAction = input.button || input.steering != 0 || input.throttle != 0;
  if (sendingAction && (display == 2 || display == 3)) {
    setStatusColor(false, true, false);
  } else {
    setStatusColor(display == 1 || display == 2,
                   display == 2 || display == 3, display == 0);
  }
  if (display != lastDisplayedStatus) {
    Serial.printf("%s (fallos de radio consecutivos: %u)\n", labels[display], failures);
    lastDisplayedStatus = display;
    drawStatusPanel(colors[display], labels[display]);
  }
}

// Redraws only what changed, capped to ~30 fps so it cannot delay the 50 Hz send.
void updateTouchUi() {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastUiFrameMs) < UI_MIN_FRAME_MS) {
    return;
  }
  static int16_t drawnKnobX = JOY_CX, drawnKnobY = JOY_CY;
  if (engaged != Zone::JOYSTICK) {
    knobX = JOY_CX;
    knobY = JOY_CY;
  }
  if (knobX != drawnKnobX || knobY != drawnKnobY) {
    lastUiFrameMs = now;
    const int16_t oldX = drawnKnobX, oldY = drawnKnobY;
    drawnKnobX = knobX;
    drawnKnobY = knobY;
    redrawJoystickRegion(oldX, oldY);
    redrawJoystickRegion(knobX, knobY);
  }
  if (input.button != buttonDrawn) {
    lastUiFrameMs = now;
    drawActionButton(input.button);
  }
}

void setup() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setStatusColor(false, false, true);  // blue: starting up

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.printf("Iniciando control CYD ESP-NOW, par %u\n", PAIR_ID);

  lcd.init();
  lcd.setRotation(1);  // landscape 320x240
  lcd.setBrightness(200);
  uiReady = true;
  showMessage("Iniciando...");

  prepareTouch();

  if (!preferences.begin("soccer_tx", false)) {
    fatalError("No se pudo abrir NVS");
  }
  reserveSequenceBlock();
  initializeRadio();
  nextSendAtUs = micros();

  drawStaticUi();
  updateStatusIndicators();
}

void loop() {
  if (!radioReady) {
    return;
  }

  const uint32_t now = micros();
  if (static_cast<int32_t>(now - nextSendAtUs) >= 0) {
    // Schedule from the current time to avoid a burst after any long pause.
    nextSendAtUs = now + SEND_INTERVAL_US;
    sampleInput();
    sendControlPacket();
    updateStatusIndicators();
    updateTouchUi();
  }
}
