# ESP32 ESP-NOW Soccer Robots

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![MCU](https://img.shields.io/badge/MCU-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Protocol](https://img.shields.io/badge/Protocol-ESP--NOW-green.svg)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html)
[![Core](https://img.shields.io/badge/Core-Arduino--ESP32%20v3.x-orange.svg)](https://github.com/espressif/arduino-esp32)
[![Robots](https://img.shields.io/badge/Team-3%20Pairs-purple.svg)]()

Sistema de control remoto seguro para robots de fútbol (soccer bots) basado en **ESP32** y **ESP-NOW**. Diseñado específicamente para prevenir sabotajes mediante cifrado nativo, autenticación por MAC y protección anti-replay.

El sistema opera con 3 pares independientes de control/robot, comunicación analógica de baja latencia (<5ms) y failsafe automático.

---

## 🛡️ Seguridad y Anti-Sabotaje

Diseñado tras incidentes de sabotaje con Bluetooth Classic sin autenticación. Este sistema implementa:

- **Cifrado AES + CMAC:** PMK y LMK únicos por par. Sin las claves correctas, los paquetes se descartan a nivel de hardware.
- **Unicast por MAC:** Cada control solo habla con su robot asignado. No hay broadcast descubrible.
- **Anti-Replay:** Contador de secuencia (`seq`) monótono. Los paquetes grabados y reenviados son ignorados.
- **Persistencia NVS:** El contador `seq` se guarda en flash cada 50 paquetes. Si el control se reinicia, recupera el último valor + margen de seguridad, evitando bloqueos sin abrir ventanas de replay.
- **Failsafe 300ms:** Si el robot pierde señal o detecta jamming, detiene motores automáticamente. No se descontrola.
- **Validación de Origen:** El robot verifica criptográficamente que el paquete venga exactamente de su control asignado.

> ⚠️ **Nota de Seguridad:** Si el *robot* se reinicia, su contador `lastSeq` vuelve a cero. En torneos locales sin sniffers RF esto es aceptable. Para entornos hostiles, considerar persistencia de `lastSeq` en robot o botón físico de armado.

---

## 🏗️ Arquitectura del Sistema

```text
Control 1 (PAIR_ID=1) ──ESP-NOW Cifrado──> Robot 1
Control 2 (PAIR_ID=2) ──ESP-NOW Cifrado──> Robot 2
Control 3 (PAIR_ID=3) ──ESP-NOW Cifrado──> Robot 3
```

- **Aislamiento:** Triple barrera (MAC + Claves únicas + Validación en código).
- **Interferencia:** Nula entre pares propios. Mismo canal WiFi soportado gracias al aislamiento criptográfico.
- **Latencia:** ~20ms (50Hz) con lectura analógica promediada y filtrada.

---

## 🔧 Hardware

### Por cada Robot (x3)
| Componente | Especificación | Notas |
| :--- | :--- | :--- |
| MCU | ESP32 DevKit | Core v3.x / IDF 5.x |
| Driver | L298N o TB6612FNG | **¡Quitar jumpers ENA/ENB en L298N!** |
| Motores | 4x DC con reductora | 2 en paralelo por lado |
| Batería | 2S LiPo 7.4V 2000mAh | Con BMS 2S y balance |
| Protección | Fusible/PTC + Capacitor 470-1000µF | En línea VM-GND |
| Filtro | Cerámico 100nF | Cerca de VCC ESP32 |
| Competencia | Switch ON/OFF + LED indicador | **Obligatorio por reglamento** |

### Por cada Control (x3)
| Componente | Especificación | Notas |
| :--- | :--- | :--- |
| MCU | ESP32 DevKit | ADC1 (GPIO34/35) para joystick |
| Input | Joystick KY-023 | Con caps 100nF en VRx/VRy-GND |
| Batería | 18650 Li-ion protegida | + TP4056 + Boost 5V |
| Carcasa | Impresa 3D / PVC | Ergonomía personalizada |

### Chasis
- **Opción A (Rápida):** Kit acrílico 2WD estándar ("Keyes Smart Car"). Requiere taladrar para LEDs/Switches.
- **Opción B (Competencia):** Impresión 3D (PLA/PETG ~150g). Permite integración nativa de switches, LEDs y gestión de cables. Modelos STL disponibles en Thingiverse/Cults3D buscando "2WD robot chassis".

---

## 💻 Firmware y Configuración

### Estructura del Repositorio
```text
esp-now-soccer-bots/
├── control_tx/          # Firmware transmisor con persistencia NVS
│   ├── control_tx.ino
│   └── team_config.h    # ⛔ NO SUBIR (contiene claves)
├── robot_rx/            # Firmware receptor con anti-replay
│   ├── robot_rx.ino
│   └── team_config.h    # ⛔ NO SUBIR
├── get_mac/             # Utilidad para obtener MACs
│   └── get_mac.ino
├── team_config.h.example # Plantilla segura para commit
├── .gitignore
├── LICENSE
└── README.md
```

### Pasos de Puesta en Marcha

1.  **Obtener MACs:** Flashear `get_mac.ino` en los 6 ESP32 y anotar direcciones.
2.  **Generar Claves:** Crear PMK/LMK de 16 bytes únicos por par.
    ```bash
    python3 -c "import secrets; print(secrets.token_hex(16))"
    ```
3.  **Configurar:** Copiar `team_config.h.example` a `team_config.h` en ambas carpetas y rellenar con MACs y claves reales.
4.  **Asignar Roles:** Definir `#define PAIR_ID 1` (o 2, 3) en cada sketch antes de flashear.
5.  **Verificar Hardware:** Confirmar que los jumpers de velocidad del L298N estén retirados.
6.  **Pruebas Unitarias:** Probar cada par individualmente verificando respuesta y failsafe (<300ms al apagar control).
7.  **Prueba Multi-Par:** Encender los 3 sistemas simultáneamente para validar aislamiento.

---

## ⚙️ Detalles Técnicos Clave

- **Joystick Filtering:** Promedio de 8 muestras + filtro RC pasabajos (100nF) para eliminar ruido RF del ESP32 sin introducir lag perceptible.
- **Control Diferencial:** Mezcla analógica X/Y → Throttle ± Steering. Deadzone configurable (default: 20).
- **PWM Real:** 5kHz, 8-bit resolución mediante `ledcAttach` (Core 3.x).
- **Flash Wear:** Escritura NVS limitada a 1Hz (cada 50 paquetes). Ajustable vía `SEQ_SAVE_INTERVAL`.
- **Alimentación:** Nunca alimentar motores desde regulador 3.3V del ESP32. Usar buck converter o regulador dedicado para lógica.

---

## 📋 Checklist Pre-Competencia

- [ ] Failsafe verificado en los 3 robots.
- [ ] Rango real probado en sede del evento.
- [ ] Consumo medido bajo carga (30 min uso intenso).
- [ ] Baterías cargadas y balanceadas noche anterior.
- [ ] Repuestos: ESP32 extra, driver, cables, soldador.
- [ ] Switches y LEDs funcionales y visibles.
- [ ] Prueba de interferencia cruzada completada.

---

## Licencia

MIT License. Ver archivo [LICENSE](LICENSE).

*Desarrollado para competencia de robótica 2026. El software se provee "tal cual", sin garantías implícitas de idoneidad para combate o sabotaje activo.*
