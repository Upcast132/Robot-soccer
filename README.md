# ESP32 ESP-NOW Soccer Robots

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![MCU](https://img.shields.io/badge/MCU-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Protocol](https://img.shields.io/badge/Protocol-ESP--NOW-green.svg)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html)
[![Core](https://img.shields.io/badge/Core-Arduino--ESP32%20v3.x-orange.svg)](https://github.com/espressif/arduino-esp32)
[![Team](https://img.shields.io/badge/Team-3%20Pairs-purple.svg)]()

Sistema de control remoto seguro para robots de fútbol basado en ESP32 y protocolo ESP-NOW. Diseñado para operar 3 pares independientes de control/robot con cifrado nativo, autenticación por MAC, protección anti-replay, failsafe automático, e indicadores de estado (LED RGB en control y robot, LCD 16x2 en el control).

## Arquitectura de Seguridad

El sistema reemplaza comunicaciones Bluetooth Classic vulnerables mediante:

1.  **Cifrado AES + CMAC:** Claves PMK y LMK únicas por par definidas en `team_config.h`. Los paquetes sin claves válidas se descartan a nivel de hardware.
2.  **Comunicación Unicast:** Cada control envía datos exclusivamente a la MAC de su robot asignado. No existe modo broadcast ni descubrimiento público.
3.  **Anti-Replay por Secuencia:** Estructura `RemoteMsg` incluye campo `seq` (uint32_t). El robot rechaza cualquier paquete con secuencia menor o igual a la última aceptada.
4.  **Persistencia NVS en Control:** El contador `seq` se almacena en flash cada 50 paquetes (~1 segundo). Al reiniciar, el control recupera el último valor y suma un margen de seguridad (`SEQUENCE_BLOCK_SIZE`), evitando bloqueos sin abrir ventanas de replay.
5.  **Failsafe de 300ms:** Si el robot no recibe paquetes válidos durante 300ms, detiene los motores inmediatamente. Protege contra jamming o pérdida de enlace.
6.  **Validación de Origen:** El callback `onDataRecv` verifica que `info->src_addr` coincida exactamente con la MAC del control configurado antes de procesar datos.

> Nota de Seguridad: Si el robot se reinicia, su contador `lastSeq` vuelve a cero. En torneos locales sin sniffers RF esto es aceptable. Para entornos hostiles, considerar persistencia de `lastSeq` en robot o botón físico de armado.

## Indicadores de Estado

Cada control y cada robot tiene un LED RGB (cátodo común) que muestra el estado del enlace sin necesidad de leer texto:

| Color | Significado en el Control | Significado en el Robot |
| :--- | :--- | :--- |
| Azul sólido | Arrancando / calibrando joystick | Arrancó, esperando el primer paquete válido |
| Verde sólido | Últimos envíos confirmados por el robot (ACK real, no solo encolado) | Enlace activo, recibiendo paquetes dentro de 300ms |
| Rojo sólido | Varios envíos seguidos sin confirmación (robot fuera de rango o apagado) | Perdió el enlace, failsafe activo (ya se había conectado antes) |
| Rojo parpadeando rápido | Error fatal (`esp_now_init()` u otra falla de arranque) | Error fatal (`esp_now_init()` u otra falla de arranque) |

El color del control usa el callback `esp_now_register_send_cb()`, que confirma entrega real a nivel de radio (ACK del peer), no solo que el paquete se encoló localmente. Esto es lo que hace que "rojo = fuera de rango" sea un dato real y no una señal que casi nunca se enciende.

Además, cada control tiene una pantalla LCD 16x2 con backpack I2C que muestra:

```
Fila 1: Control PAIR X
Fila 2: Enlazado / Sin senal / ERROR FATAL
```

La fila 2 se actualiza cada ~300ms o al cambiar de estado, lo que ocurra primero, para no meter latencia en el ciclo de envío de 20ms.

> Advertencia eléctrica: El backpack I2C del LCD debe alimentarse desde el pin **3.3V** del ESP32, no desde 5V/VIN. La mayoría de los backpacks PCF8574 tienen pull-ups de SDA/SCL hacia su propio VCC; a 5V esas líneas superan el máximo absoluto de los GPIO del ESP32 (~3.6V) y pueden dañarlos. A 3.3V funciona correctamente, solo el backlight queda un poco más tenue.

## Diagramas de Conexion

### Control Remoto (Transmisor)

```text
JOYSTICK KY-023          ESP32 DEVKIT
+-----------+            +----------------+
| VCC       |------------| 3V3            |
| GND       |-----+------| GND            |
| VRx       |--+  |      |                |
|           |  |  |      |                |
| VRy       |-+|--+------| GPIO34 (ADC1)  | <-- Cap 100nF a GND
|           | ||         |                |
| SW        | |+---------| GPIO27         | <-- INPUT_PULLUP
+-----------+ ||         |                |
              |+---------| GPIO35 (ADC1)  | <-- Cap 100nF a GND
              |          |                |
ALIMENTACION  |          | VIN            |
TP4056+BOOST  |          | GND            |
5V OUT -------+----------|                |
GND -----------+---------|                |
```

```text
LED RGB (catodo comun)      ESP32 DEVKIT
+--------------------+      +----------------+
| R (anodo)          |--[R]-| GPIO25         |
| G (anodo)          |--[R]-| GPIO26         |
| B (anodo)          |--[R]-| GPIO33         |
| Catodo comun       |------| GND            |
+--------------------+      +----------------+
[R] = resistencia 220-330 ohm en cada pata de color
```

```text
LCD 16x2 + BACKPACK I2C     ESP32 DEVKIT
+--------------------+      +----------------+
| GND                |------| GND            |
| VCC                |------| 3V3 (NO 5V/VIN)|
| SDA                |------| GPIO21         |
| SCL                |------| GPIO22         |
+--------------------+      +----------------+
```

Notas criticas del control:
- GPIO34 y GPIO35 pertenecen a ADC1. Funcionan correctamente con WiFi/ESP-NOW activo (ADC2 se deshabilita al usar WiFi).
- Los capacitores ceramicos de 100nF deben soldarse lo mas cerca posible de los pines del ESP32 para filtrar ruido RF.
- El joystick se alimenta a 3.3V. Nunca conectar a 5V (danaria el ADC del ESP32).
- GPIO27 usa resistencia pull-up interna. El boton del joystick conecta a GND cuando se presiona.
- El LED RGB y el LCD comparten GND con el resto del circuito.
- El backpack del LCD se alimenta a 3.3V, no a 5V (ver advertencia electrica arriba).
- Si el LCD no responde en direccion 0x27 ni 0x3F, el firmware continua sin pantalla (no bloquea el control); revisar la direccion I2C real con un sketch escaner si persiste.

### Robot (Receptor)

```text
ESP32 DEVKIT             DRIVER L298N               MOTORES
+----------------+       +------------------+       +-----------+
| GPIO25 (IN1)   |-------| IN1              |       |           |
| GPIO26 (IN2)   |-------| IN2              |       | MOTOR IZQ |
| GPIO32 (ENA)   |-------| ENA (SIN JUMPER!)|-------| (1 motor) |
| GPIO27 (IN3)   |-------| IN3              |       |           |
| GPIO14 (IN4)   |-------| IN4              |       | MOTOR DER |
| GPIO33 (ENB)   |-------| ENB (SIN JUMPER!)|-------| (1 motor) |
| GND            |---+---| GND              |       +-----------+
+----------------+   |   +--------+---------+
                     |            |
BATERIA 2S 18650 (BMS con balance)
BAT+ ------------------------------| 12V / VM
BAT- -----------------------+------| GND
                            |
CAPACITOR 470-1000uF         |      BUCK 5V DEDICADO
(+) a VM, (-) a GND          +------| IN-
BAT+ -------------------------------| IN+
                                   | OUT+ |------ ESP32 VIN
                                   | OUT- |------ ESP32 GND
```

```text
LED RGB (catodo comun)      ESP32 DEVKIT
+--------------------+      +----------------+
| R (anodo)          |--[R]-| GPIO17         |
| G (anodo)          |--[R]-| GPIO16         |
| B (anodo)          |--[R]-| GPIO4          |
| Catodo comun       |------| GND            |
+--------------------+      +----------------+
[R] = resistencia 220-330 ohm en cada pata de color
```

Advertencias criticas del robot:
- RETIRAR JUMPERS DE ENA Y ENB: Si los jumpers estan puestos, los pines quedan fijos en HIGH y el PWM del ESP32 no tendra efecto. La velocidad sera siempre maxima o nula.
- Tierra comun: GND de bateria, driver, LED RGB y ESP32 deben estar conectados entre si.
- No alimentar motores desde el pin 3.3V o 5V del ESP32.
- Alimentar el ESP32 con un buck dedicado de 5V. No usar la salida 5V del L298N con este paquete 2S.
- El paquete 2S requiere dos celdas 18650 iguales, BMS 2S con balance y cargador 8.4V para 2S. No cargarlo con TP4056.
- Capacitor electrolitico de 470-1000uF entre VM y GND es obligatorio para absorber picos de corriente y evitar resets del ESP32.
- Fusible o PTC en positivo de bateria recomendado para proteccion contra cortocircuitos.
- GPIO16, GPIO17 y GPIO4 quedaron libres tras el cableado del driver; si se cambia el pinout del L298N, reasignar el LED RGB a otros pines libres.

## Lista de Materiales (BOM)

### Por cada Robot (Cantidad total para 3 robots)

| Componente | Cantidad por unidad | Total (3 unidades) | Especificacion / Notas |
| :--- | :--- | :--- | :--- |
| ESP32 DevKit | 1 | 3 | WROOM-32 o similar |
| Driver L298N | 1 | 3 | O TB6612FNG como mejora futura |
| Motor DC con reductora | 2 | 6 | 3-6V, eje D. Un motor por canal del L298N |
| Celda 18650 3.7V nominal / 4.2V cargada | 2 | 6 | Celdas iguales en serie: 2S = 7.4V nominal, 8.4V maximo |
| BMS 2S 7.4V con balance | 1 | 3 | Proteccion sobre-descarga y balance de celdas |
| Convertidor buck 5V | 1 | 3 | Alimentacion dedicada para VIN del ESP32 |
| Capacitor electrolitico | 1 | 3 | 470uF a 1000uF, 16V+. Entre VM-GND del driver |
| Capacitor ceramico | 1 | 3 | 100nF. Cerca de VCC del ESP32 |
| Fusible o PTC | 1 | 3 | En positivo de bateria. Valor segun stall current |
| Conector JST-XH | 1 | 3 | Para balance de bateria |
| Conector JST-SM/T | 2 | 6 | Alimentacion principal y carga |
| Switch SPDT | 1 | 3 | Obligatorio por reglamento de competencia |
| **LED RGB (catodo comun)** | 1 | 3 | Indicador de estado (azul/verde/rojo). Obligatorio por reglamento |
| **Resistencia 220-330 ohm** | 3 | 9 | Una por cada pata de color del LED RGB |
| Rueda loca | 1 | 3 | Metalica o plastica con rodamiento |
| Chasis | 1 | 3 | Acrilico 2WD o impresion 3D PLA/PETG |

### Por cada Control (Cantidad total para 3 controles)

| Componente | Cantidad por unidad | Total (3 unidades) | Especificacion / Notas |
| :--- | :--- | :--- | :--- |
| ESP32 DevKit | 1 | 3 | WROOM-32 o similar |
| Joystick KY-023 | 1 | 3 | Modulo analogico 2 ejes + boton |
| Capacitor ceramico | 2 | 6 | 100nF. Uno en VRx-GND, otro en VRy-GND |
| Celda 18650 Li-ion | 1 | 3 | Protegida. Samsung/LG/Molicel recomendadas |
| Modulo TP4056 + Boost 5V | 1 | 3 | Carga 1S + elevacion a 5V para ESP32 |
| Portapilas 18650 | 1 | 3 | Con switch integrado preferiblemente |
| Switch ON/OFF | 1 | 3 | Si modulo combo no lo incluye |
| **LED RGB (catodo comun)** | 1 | 3 | Indicador de estado del enlace (azul/verde/rojo) |
| **Resistencia 220-330 ohm** | 3 | 9 | Una por cada pata de color del LED RGB |
| **Pantalla LCD 16x2 con backpack I2C** | 1 | 3 | Muestra PAIR_ID y estado del enlace. Alimentar a 3.3V, no 5V |
| Carcasa | 1 | 3 | Impresion 3D o caja plastica PVC |

### Herramientas compartidas (no por unidad)

| Herramienta | Cantidad | Uso |
| :--- | :--- | :--- |
| Multimetro | 1 | Medir corriente stall, verificar continuidad |
| Soldador + estaño | 1 | Conexiones permanentes |
| Heatshrink | Varios | Aislamiento de uniones soldadas |
| Taladro/Dremel | 1 | Solo si chassis es acrilico (para LEDs/switches) |
| Programador USB | 1 | Solo si ESP32 no tiene USB integrado |
| Cargador balance 2S | 1 | iMax B6 o similar. TP5100 como alternativa simple |

## Librerias Adicionales Requeridas

Ademas del core Arduino-ESP32, el control necesita:

- **LiquidCrystal_I2C** (por Frank de Brabander o equivalente): Arduino IDE → Library Manager → buscar "LiquidCrystal I2C" → instalar.

La direccion I2C del backpack varia segun el chip: el firmware prueba automaticamente `0x27` y luego `0x3F` al arrancar. Si tu backpack usa otra direccion, corre un sketch escaner I2C estandar para encontrarla y agregala en `initializeLcd()`.

El robot no necesita librerias nuevas; el LED RGB usa `digitalWrite()` estandar.

## Estructura del Repositorio

```text
esp-now-soccer-bots/
├── soccer_protocol.h       # Formato de paquete y CRC compartidos
├── control_tx/
│   ├── control_tx.ino       # Firmware transmisor: NVS, LED RGB, LCD I2C
│   └── team_config.h        # NO SUBIR: contiene claves y MACs reales
├── robot_rx/
│   ├── robot_rx.ino         # Firmware receptor: anti-replay, LED RGB
│   └── team_config.h        # NO SUBIR: contiene claves y MACs reales
├── get_mac/
│   └── get_mac.ino          # Utilidad para leer MAC de cada ESP32
├── team_config.h.example    # Plantilla segura para versionar
├── .gitignore               # Excluye team_config.h real
├── LICENSE                  # MIT License
└── README.md                # Este archivo
```

## Configuracion Inicial

### Paso 1: Obtener MACs

Flashear `get_mac.ino` en cada uno de los 6 ESP32 individualmente. Abrir monitor serial a 115200 baudios y anotar la direccion MAC mostrada.

### Paso 2: Generar claves PMK/LMK

Generar 3 pares de claves de 16 bytes cada una. No usar las del ejemplo.

```bash
python3 -c "import secrets; print(secrets.token_hex(16))"
```

Ejecutar 6 veces (3 PMK + 3 LMK). Cada par debe tener claves distintas.

### Paso 3: Configurar team_config.h

Copiar `team_config.h.example` a `team_config.h` dentro de las carpetas `control_tx/` y `robot_rx/`. Rellenar con:
- Las 6 MACs reales obtenidas en paso 1.
- Las 6 claves generadas en paso 2.
- Canal WiFi deseado (default: 1). Canales recomendados si hay interferencia: 1, 6 u 11.

Las copias contienen secretos y estan excluidas por `.gitignore`. La plantilla versionada usa MAC y claves publicas de ejemplo; no proporcionan seguridad.

### Paso 4: Asignar PAIR_ID

En cada sketch, definir el identificador de par antes de compilar:

| Dispositivo | PAIR_ID | Archivo |
| :--- | :--- | :--- |
| Control 1 | 1 | control_tx/control_tx.ino |
| Robot 1 | 1 | robot_rx/robot_rx.ino |
| Control 2 | 2 | control_tx/control_tx.ino |
| Robot 2 | 2 | robot_rx/robot_rx.ino |
| Control 3 | 3 | control_tx/control_tx.ino |
| Robot 3 | 3 | robot_rx/robot_rx.ino |

### Paso 5: Verificar hardware critico

Antes de encender:
- Confirmar que jumpers ENA/ENB del L298N estan retirados.
- Verificar polaridad de bateria y capacitor electrolitico.
- Confirmar tierra comun entre todos los componentes.
- Verificar que LEDs y switches estan instalados (requisito de competencia).
- Confirmar que el backpack del LCD esta alimentado a 3.3V, no a 5V.
- Confirmar que las 3 patas de cada LED RGB tienen su resistencia individual.

### Paso 6: Pruebas unitarias

Para cada par, individualmente:
1. Encender robot. Verificar LED azul (esperando control).
2. Encender control. Verificar LED azul mientras calibra, luego verde. LCD muestra "Control PAIR X" y "Enlazado".
3. Mover joystick en todas direcciones. Verificar respuesta correcta.
4. Presionar el boton del joystick y verificar que actua como parada mientras se mantiene pulsado.
5. Apagar control abruptamente. Verificar que el L298N entra en frenado en menos de 300ms y el LED del robot pasa a rojo.
6. Reiniciar control. Verificar que robot acepta comandos inmediatamente (gracias a persistencia NVS) y ambos LEDs vuelven a verde.
7. Alejar el control del robot hasta perder rango. Verificar que el LED y el LCD del control cambian a "Sin senal" tras varios envios fallidos seguidos.
8. Medir voltaje y corriente de cada motor bajo carga antes de aumentar `MOTOR_MAX_DUTY`.

### Paso 7: Prueba multi-par

Encender los 3 controles y 3 robots simultaneamente en el mismo espacio fisico. Verificar que:
- Control 1 solo mueve Robot 1, y su LCD muestra "Control PAIR 1".
- Control 2 solo mueve Robot 2, y su LCD muestra "Control PAIR 2".
- Control 3 solo mueve Robot 3, y su LCD muestra "Control PAIR 3".
- No existe interferencia cruzada ni retrasos anormales.

## Parametros Tecnicos del Firmware

| Parametro | Valor | Ubicacion |
| :--- | :--- | :--- |
| Frecuencia de envio | 50 Hz (20ms) | control_tx.ino loop() |
| Timeout failsafe | 300 ms | robot_rx.ino FAILSAFE_TIMEOUT_MS |
| Reserva del contador NVS | 50 secuencias (~1s) | control_tx.ino SEQUENCE_BLOCK_SIZE |
| Limite PWM inicial | 200 de 255 | robot_rx.ino MOTOR_MAX_DUTY |
| Resolucion PWM | 8 bits (0-255) | robot_rx.ino PWM_RESOLUTION_BITS |
| Frecuencia PWM | 5000 Hz | robot_rx.ino PWM_FREQUENCY_HZ |
| Deadzone joystick | 50 de 1000 | control_tx.ino JOYSTICK_DEADZONE |
| Promedio muestras ADC | 8 lecturas | control_tx.ino readAveraged() |
| Canal WiFi | Configurable | team_config.h WIFI_CHANNEL |
| Umbral de fallos para LED/LCD rojo | 10 envios seguidos (~200ms) | control_tx.ino SEND_FAILURE_THRESHOLD |
| Intervalo de refresco del LCD | 300 ms | control_tx.ino LCD_UPDATE_INTERVAL_MS |
| Direcciones I2C del LCD probadas | 0x27, luego 0x3F | control_tx.ino initializeLcd() |

## Opciones de Chasis

### Opcion A: Kit acrilico 2WD estandar
- Busqueda: "2WD Robot Chassis Kit Arduino" o "Keyes 2WD Smart Car".
- Incluye: base acrilica, soportes motor, ruedas, rueda loca, tornilleria.
- Requiere: taladrar orificios para LEDs, switches y gestion de cables.
- Ventaja: entrega rapida (1-2 dias), economico, facil reparacion.
- Desventaja: modificacion mecanica requerida para requisitos de competencia.

### Opcion B: Impresion 3D personalizada
- Modelos STL: buscar "2WD robot chassis 3D print STL two motors" en Thingiverse/Cults3D.
- Material: PLA o PETG. ~150g por chassis.
- Tiempo impresion: 4-8 horas por unidad.
- Ventaja: orificios nativos para LEDs/switches, diseño optimizado, iteraciones rapidas.
- Desventaja: requiere acceso a impresora 3D, tiempo de produccion mayor.

## Checklist Pre-Competencia

- [ ] Failsafe verificado en los 3 robots (detencion < 300ms).
- [ ] Rango real probado en sede del evento.
- [ ] Consumo medido bajo carga durante 30 minutos de uso intenso.
- [ ] 2 juegos de baterias cargadas y balanceadas por robot.
- [ ] Repuestos disponibles: ESP32 extra, driver, cables, soldador.
- [ ] Switches y LEDs funcionales y visibles segun reglamento.
- [ ] Prueba de interferencia cruzada completada con 3 pares activos.
- [ ] Jumpers ENA/ENB retirados en todos los drivers L298N.
- [ ] Capacitores instalados en posiciones correctas.
- [ ] LED RGB de cada control y cada robot cambia de color correctamente (azul/verde/rojo).
- [ ] LCD de cada control muestra el PAIR_ID correcto y responde a cambios de estado.
- [ ] Backpack de cada LCD confirmado a 3.3V, no a 5V.

## Mejoras Futuras

- Migracion a TB6612FNG para reducir perdidas por calor y mejorar eficiencia.
- Encoders + PID para correccion de trayectoria y control preciso.
- Telemetria de retorno real: voltaje de bateria y RSSI del robot enviados al control y mostrados en el LCD (distinto del estado de enlace ya implementado, que solo refleja la vista del propio control).
- Persistencia de lastSeq en robot para cerrar ventana de replay tras reinicio.
- Boton fisico de armado como requisito adicional de seguridad.

## Licencia

MIT License. Ver archivo LICENSE.

El software se provee "tal cual", sin garantias implicitas de idoneidad para combate, sabotaje activo o cumplimiento de reglamentos de competencia especificos. La seguridad implementada es adecuada para torneos locales sin adversarios con equipamiento de sniffing RF profesional.
