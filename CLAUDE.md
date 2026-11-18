# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Proyecto

Firmware Arduino para ESP32 que controla 3 pares independientes control/robot de fútbol mediante ESP-NOW cifrado (PMK/LMK). La documentación del repositorio (README, `RESUMEN_*.txt`, `TODO_CHECKLIST_OPERACIONAL.txt`) está en español; mantener ese idioma al documentar. Pruebas en hardware realizadas hasta ahora: solo el par 3 (CYD + ESP32 sin motores); el resto del firmware sigue sin validación física. No afirmar más validación de la que se observó.

## Comandos

Desde la raíz del repositorio (toolchain verificado: Arduino-ESP32 3.3.11, FQBN `esp32:esp32:esp32`):

```sh
arduino-cli lib install "LiquidCrystal I2C@1.1.2"   # dependencia de control_tx
arduino-cli lib install LovyanGFX                   # dependencia de control_cyd y tools/cyd_probe
arduino-cli compile --fqbn esp32:esp32:esp32 --warnings all --build-path build/control_tx control_tx
arduino-cli compile --fqbn esp32:esp32:esp32 --warnings all --build-path build/control_cyd control_cyd
arduino-cli compile --fqbn esp32:esp32:esp32 --warnings all --build-path build/robot_rx robot_rx
node tests/simulate_safety.cjs                       # sin dependencias; no admite filtrar grupos
```

Cargar: `arduino-cli upload -p <puerto> --fqbn esp32:esp32:esp32 --input-dir build/<sketch> <sketch>`. Para dejar un monitor serie sin reiniciar la placa hay que abrir el puerto con DTR/RTS en falso, y aun así algunas placas se reinician al abrirlo.

Sin `team_config.h`, los sketches compilan con `team_config.example.h` y emiten un `#warning` esperado. La única "suite de tests" es `tests/simulate_safety.cjs`: modelos lógicos en JS que **no** ejecutan el C++, pero leen constantes (`constexpr ... = <decimal>;`) directamente de los `.ino` mediante regex y comprueban fragmentos de código con expresiones regulares. Renombrar esas constantes, escribirlas en otro formato (hex, expresiones) o mover el contador/LED del callback rompe la simulación; actualizar ambos lados juntos.

## Arquitectura

- `control_tx/control_tx.ino` — mando con joystick KY-023: calibra el joystick (radio apagada durante la calibración), envía `RemoteMsg` a 50 Hz, reserva bloques de secuencia en NVS (`soccer_tx/next_seq`, bloques de 10 000, se persisten **antes** de usarse), muestra estado en LED RGB y LCD I2C. Solo muestra "Armado"/verde al recibir un `StatusMsg` válido del robot, nunca por el ACK de envío.
- `control_cyd/control_cyd.ino` — variante del mando para la CYD ESP32-2432S024 (LovyanGFX): **joystick virtual táctil** y botón táctil ARMAR/FRENO. Reutiliza sin cambios la lógica de radio, secuencia NVS y confirmación de `control_tx`; solo cambian entrada y salida. Sin contacto = ejes en neutral. El táctil resistivo es monotáctil. Calibración táctil guardada en NVS (`cyd_ui/touch`, independiente de `soccer_tx`); mantener tocada la pantalla al arrancar para recalibrar. El LED RGB integrado (activo en bajo) queda en **verde mientras se envía una acción** (joystick fuera de centro o botón) con enlace confirmado (Desarmado/Armado); si no, muestra el color del estado.
- `robot_rx/robot_rx.ino` — robot: valida cada paquete en el callback ESP-NOW (MAC, magic, versión, PAIR_ID, CRC, rangos, secuencia modular más nueva), procesa el gesto de armado, failsafe a 300 ms, rampa de motores (40 unidades/10 ms, pausa de 20 ms al invertir) y responde `StatusMsg`. Frenado (botón, failsafe, error fatal) omite la rampa. El **LED integrado (GPIO2) da un pulso de 8 ms por cada comando aceptado** (`RX_LED_PULSE_MS`, debe ser menor que 20 ms): el callback solo incrementa `acceptedPacketCount` bajo `commandMux` y `loop()` hace el GPIO.
- Modelo de concurrencia: los callbacks ESP-NOW solo validan y copian datos bajo secciones críticas; PWM, LCD/TFT, LEDs, envío de estado y `Serial` se hacen en `loop()`. Una petición de frenado (`brakePending`) se conserva hasta que `loop()` la atienda. Respetar esta separación al modificar.
- `soccer_protocol.h` — formato de cable (protocolo v2): `RemoteMsg` 18 bytes y `StatusMsg` 14 bytes empaquetados con `static_assert` de tamaño, CRC16-CCITT. Cambiar el formato exige subir la versión y actualizar control y robot a la vez.
- `control_tx/esp_now_compat.h` (y su copia en `control_cyd/`) — selecciona la firma del callback de envío según `ESP_IDF_VERSION` (cambió en IDF 5.5). Debe estar en un header para que los prototipos generados por Arduino la vean.
- `get_mac/get_mac.ino` — utilidad para leer la MAC de cada ESP32.
- `tools/cyd_probe/` — sondeo de hardware de la CYD (MAC, escaneo I2C, lectura cruda de CST820 y de dos cableados del XPT2046). No es firmware del robot. Los structs usados como parámetros de función en un `.ino` fallan por el auto-prototipo de Arduino: usar tipos simples.

## Hardware verificado: CYD ESP32-2432S024 (resistiva)

Verificado con `tools/cyd_probe` el 18-nov-2026: pantalla ILI9341 (SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, luz 27, `invert=false`, `rgb_order=false`), **táctil XPT2046 en el mismo bus SPI que la pantalla** (CS 33, IRQ 36; los pines 25/32/39 de otras CYD no responden), LED RGB activo en bajo (R 4, G 16, B 17). No hay CST820 funcional (el proyecto `~/cyd-claude-monitor` asumía capacitivo y no lo encontraba). En LovyanGFX, panel y táctil se configuran con `bus_shared = true` y `spi_host = SPI2_HOST`.

## Configuración de pares (team_config.h)

- Cada dispositivo usa solo la fila de `TEAM_PAIRS` que indica su `PAIR_ID`; el mismo archivo con las 3 filas sirve para el control y el robot de un par (en `control_cyd/` y `robot_rx/` son idénticos para el par 3). Las filas no usadas pueden quedar con valores de ejemplo.
- Procedimiento seguido para el par 3: leer la MAC de cada placa (`esptool flash-id` o `get_mac`), escribirlas en la fila 3 (MAC control, MAC robot), generar PMK y LMK nuevas con `secrets.token_bytes(16)` **sin imprimirlas**, copiar el mismo archivo a la otra carpeta, compilar y cargar ambos.
- Al comprobar un `team_config.h` no mostrar las líneas de claves (ni siquiera con `cut`/`sed`): si se muestran bytes de una PMK/LMK, regenerarlas.
- Estado actual: par 3 = CYD (control) + ESP32 desnudo (robot, sin motores/L298N). Verificado por Serial: Esperando robot → Desarmado → Armado en ambos. Pares 1 y 2 usan `control_tx`/`robot_rx` con sus propios `team_config.h`, que no están en este repositorio ni en esta máquina.

## Reglas importantes

- **Cabeceras duplicadas a propósito**: `soccer_protocol.h` y `team_config.example.h` existen en `control_tx/`, `control_cyd/` y `robot_rx/` (y `esp_now_compat.h` en las dos carpetas de control); cada carpeta de sketch debe ser autocontenida. Deben ser idénticas; editar todas las copias. El test lo verifica.
- `team_config.h` (MACs y claves reales) está en `.gitignore` y nunca debe versionarse ni imprimirse. `PAIR_ID` se define ahí (línea 9); los `#define PAIR_ID` de respaldo en los `.ino` solo sirven para compilar con la plantilla. El README referencia números de línea de la plantilla: si se modifica, actualizar el README. No escribir MACs reales ni contraseñas en documentación ni commits.
- No proponer borrar NVS para resolver desconexiones (rompe la garantía anti-replay).
- Antes de reemplazar el firmware de una placa que el usuario ya usa para otro proyecto, guardar su flash completa (`esptool read-flash 0 0x400000`); la copia de la CYD está fuera del repo.
- En este equipo el puerto serie es `root:uucp`; si se recrea el nodo `/dev/ttyUSB*` hay que volver a dar acceso (ACL con `setfacl` o grupo `uucp`).
- Al cambiar comportamiento, mantener sincronizados README.md, `TODO_CHECKLIST_OPERACIONAL.txt` y los resúmenes cuando describan ese comportamiento.
