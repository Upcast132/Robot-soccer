# ESP32 ESP-NOW Soccer Robots

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![MCU](https://img.shields.io/badge/MCU-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Protocol](https://img.shields.io/badge/Protocol-ESP--NOW-green.svg)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html)
[![Core](https://img.shields.io/badge/Core-Arduino--ESP32%20v3.x-orange.svg)](https://github.com/espressif/arduino-esp32)
[![Team](https://img.shields.io/badge/Team-3%20Pairs-purple.svg)]()

Sistema de control remoto seguro para robots de fútbol basado en ESP32 y protocolo ESP-NOW. Diseñado para operar 3 pares independientes de control/robot con cifrado nativo, autenticación por MAC, protección anti-replay, failsafe automático, e indicadores de estado (LED RGB en control y robot, LCD 16x2 en el control).

## Estado de esta revisión

Se implementa el protocolo de aplicación **v2**. **Actualizar control y robot juntos**: las versiones incompatibles se rechazan. Compatibilidad prevista con Arduino-ESP32 3.x; **no se compiló, no se instaló un entorno y no se probó ni cargó firmware en hardware** en esta revisión.

Se revisaron las firmas oficiales del callback de envío en [ESP-IDF 5.4](https://github.com/espressif/esp-idf/blob/v5.4/components/esp_wifi/include/esp_now.h) y [ESP-IDF 5.5](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_wifi/include/esp_now.h). `esp_now_compat.h` selecciona `const uint8_t *` antes de 5.5 y `const esp_now_send_info_t *` desde 5.5 mediante `ESP_IDF_VERSION`. Esta revisión de cabeceras no demuestra que los sketches compilen.

## Protocolo y seguridad

- Unicast cifrado en ambos sentidos con PMK/LMK del par; MAC de origen y destino, versión, identificador de mensaje, PAIR_ID y CRC se validan antes de procesar.
- `RemoteMsg` conserva sus campos y tamaño de **18 bytes**: magic (4), version (1), pairId (1), flags (2), seq (4), throttle (2), steering (2), crc16 (2). Identificador de comando: `SOCCER_COMMAND_MAGIC = 0x534F4343`.
- `StatusMsg` tiene **14 bytes**: magic (4), version (1), pairId (1), state (1), reserved (1, cero), acceptedSeq (4), crc16 (2). Identificador de estado: `SOCCER_STATUS_MAGIC = 0x534F4353`. Estados: espera=0, desarmado=1, armado=2.
- Ambos mensajes están empaquetados, usan enteros de ancho fijo en el orden nativo little-endian del ESP32 y CRC16 CCITT (inicio 0xFFFF, polinomio 0x1021) sobre los bytes anteriores al CRC. El CRC detecta errores; la protección criptográfica corresponde a ESP-NOW.
- Solo se aceptan ejes entre −1000 y 1000 y flags conocidos (bit 0: botón). Un paquete inválido, duplicado o atrasado no renueva el failsafe ni avanza el gesto de armado.
- La comparación modular acepta una distancia de secuencia entre 1 y 2³¹−1, incluido el paso de UINT32_MAX a cero. El receptor conserva su última secuencia al perder señal.
- El control guarda en `soccer_tx/next_seq` el final exclusivo de un bloque **antes de usarlo**. Reserva 10 000 secuencias: aproximadamente una escritura cada 200 s a 50 Hz, más la reserva al arrancar. Tras reiniciar comienza en el final del bloque anterior y reserva otro; puede saltarse hasta 10 000 números, sin reutilizar los ya emitidos. Se conserva la clave existente, incluso al migrar desde bloques de 50.
- El desbordamiento es modular; cero es válido y no se reinicia arbitrariamente a uno. Como en cualquier comparación de media ventana, saltos acumulados de 2³¹ o más sin recepción no se pueden ordenar. No borrar NVS para resolver una desconexión.
- El robot responde como máximo cada 100 ms en régimen estable y además al cambiar de estado, usando la última secuencia aceptada. El control exige avance y correspondencia con uno de sus últimos 32 envíos, realizado hace como máximo 300 ms. Estados duplicados, antiguos o de otra ejecución no confirman un enlace nuevo.
- Las secciones críticas protegen los datos compartidos. El receptor valida cada gesto en el callback y conserva una petición de frenado hasta que `loop()` la atienda, aunque otro paquete haya reemplazado el comando. LCD, PWM, envío de estado y diagnósticos se ejecutan fuera de callbacks y bloqueos.

El último contador del robot vive en RAM: un reinicio pierde el historial anti-replay del receptor. El armado no sustituye una autenticación de sesión ni cierra esa limitación. Las MAC y claves reales deben permanecer fuera de Git; esta revisión no crea configuraciones privadas ni borra NVS.

## Calibración, armado y parada

1. Al encender el control, dejar ambos ejes centrados. El LCD indica **Centrar joystick**. Soltar el botón y pulsarlo para iniciar la captura.
2. Se toman 128 lecturas por eje. Ambos promedios deben estar entre **1536 y 2560** inclusive; el máximo menos el mínimo de las lecturas debe ser **≤100 ADC** en cada eje. Los límites son constantes ajustables.
3. Si falla, aparece **Recalibrar**. Soltar el botón; tras un segundo vuelve la indicación de centrar y se permite otra pulsación. Durante toda la calibración la radio aún no está inicializada y no se envían órdenes.
4. Si pasa, aparece **Soltar boton**. Se exige una liberación estable de 30 ms antes de iniciar la radio. La pulsación de calibración no cuenta para armar.
5. Con enlace confirmado y **Desarmado**, mantener ambos ejes en neutral y el botón suelto durante **500 ms**, pulsar y soltar sin abandonar neutral. El robot valida el gesto con paquetes nuevos. Neutral significa valor absoluto de cada eje menor que 50 en la escala normalizada.
6. Solo **Armado** permite movimiento. Pulsar el botón aplica frenado sin rampa al procesar el paquete y deja el robot desarmado. Soltar con el joystick inclinado no reanuda el movimiento.
7. Al superar **300 ms** sin un comando válido nuevo, el robot frena y vuelve a espera. Al recuperar paquetes queda desarmado: es obligatorio repetir el gesto completo. Desplazar cualquier eje durante el intento o perder señal lo cancela.

El failsafe se atiende en la siguiente iteración después de superar 300 ms; no se promete una detención mecánica en menos de 300 ms. La pulsación debe llegar en un paquete: el botón remoto no sustituye un corte físico de alimentación. Un error fatal del robot frena antes del diagnóstico; si el control deja de enviar por un error fatal, el robot depende del failsafe.

## Indicadores de estado

| Color | Control | Robot |
| :--- | :--- | :--- |
| Azul sólido | Inicio/calibración o **Esperando robot**, todavía sin confirmación válida | Espera el primer comando válido |
| Amarillo (rojo + verde) | **Desarmado**, respuesta reciente sin permiso de movimiento | Paquetes recientes, desarmado |
| Verde sólido | **Armado**, confirmado por el firmware del robot | Armado y con comandos recientes |
| Rojo sólido | **Sin senal**, más de 300 ms sin una confirmación nueva tras haber recibido alguna | Más de 300 ms sin comando nuevo tras haberse conectado |
| Rojo intermitente rápido | **ERROR FATAL** | Error fatal con freno aplicado |

El éxito del callback de envío queda como diagnóstico de radio; no habilita el verde. Al arrancar con el robot apagado el LCD permanece en **Esperando robot**, aunque la radio local esté lista. La confirmación puede demorarse respecto al estado local del robot; una pérdida solo en el retorno puede mostrar rojo en el control mientras el robot todavía recibe comandos.

El LCD conserva `Control PAIR X` en la primera fila. La segunda muestra los mensajes de calibración y enlace anteriores; se refresca cada 300 ms o al detectar un cambio. Si no hay LCD, seguir las indicaciones por Serial.

> El backpack I2C no debe elevar SDA/SCL a 5 V. El cableado propuesto usa VCC de 3.3 V; comprobar que el módulo concreto funciona a esa tensión. Un módulo que requiera 5 V necesita adaptación de nivel adecuada.

## Movimiento progresivo

La mezcla diferencial mantiene la reescala proporcional a ±1000. Cada motor cambia como máximo **40 unidades de mando cada 10 ms**. Para invertir, baja hasta cero y permanece allí **20 ms** antes de aplicar el sentido contrario. La pausa también se conserva si el objetivo cambia mientras está a cero. No se acumulan pasos para ejecutarlos de golpe tras una demora.

El botón, el failsafe y un error fatal del robot omiten la rampa: frenan y borran objetivos y progreso pendientes. Se mantienen PWM a 5 kHz y 8 bits, mínimo 60 y máximo 200 de 255 en movimiento; cero aplica el frenado activo existente. La rampa limita el mando, no garantiza una variación uniforme del par: el primer mando distinto de cero aún aplica el PWM mínimo. La respuesta eléctrica y mecánica debe medirse.

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
| **LED RGB (catodo comun)** | 1 | 3 | Indicador de estado (azul/amarillo/verde/rojo). Obligatorio por reglamento |
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
| **LED RGB (catodo comun)** | 1 | 3 | Indicador de estado del enlace (azul/amarillo/verde/rojo) |
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
├── soccer_protocol.h       # Comandos v2, estados y CRC compartidos
├── esp_now_compat.h        # Firma de envío según ESP-IDF
├── tests/simulate_safety.cjs # Simulaciones lógicas con Node.js, sin compilar
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

### Paso 6: Pruebas manuales pendientes

Realizar primero con las ruedas levantadas, por cada par:

1. Encender solo el control y completar calibración: nunca debe indicar Armado ni enlace confirmado; debe mostrar Esperando robot.
2. Calibrar con joystick inclinado y después moviéndolo durante la captura: debe pedir Recalibrar, permitir otro intento y no enviar comandos.
3. Arrancar el robot con el joystick desplazado: permanecer frenado. Centrar, esperar 500 ms con botón suelto, pulsar y soltar en neutral; comprobar Armado.
4. Interrumpir el gesto con movimiento o pérdida de señal, incluido entre pulsación y liberación: no debe armar.
5. Armado y en movimiento, pulsar el botón; mantener joystick inclinado al soltar. Debe frenar y permanecer desarmado hasta otro gesto completo.
6. Apagar el control o interrumpir los paquetes durante más de 300 ms. Medir cuándo se aplica el freno y cuándo paran las ruedas. Reconectar o reiniciar el control: recalibrar si reinició y repetir el armado.
7. Inyectar en un banco controlado duplicados, CRC incorrecto, flags desconocidos, ejes fuera de rango y versión anterior. Ninguno debe renovar el failsafe. Repetir con estados duplicados/antiguos: no deben renovar la confirmación del LCD.
8. Interrumpir únicamente el retorno robot→control: tras 300 ms sin estado nuevo, el control debe indicar Sin senal. Medir el comportamiento con pérdida y reordenamiento.
9. Verificar subida gradual e inversión en ambos motores: paso por cero, pausa mínima de 20 ms y frenado sin rampa durante aceleración, desaceleración y pausa.
10. Verificar reinicio del control cerca del final de un bloque NVS y avance modular cerca de UINT32_MAX en un banco de pruebas, sin borrar NVS del equipo operativo.
11. Medir latencia real, dispersión ADC de cada joystick, tensión/corriente de motor, frenado y temperatura del driver bajo carga antes de ajustar parámetros.

### Paso 7: Prueba multi-par

Encender los 3 controles y 3 robots, calibrar y armar cada par en el mismo espacio fisico. Verificar que:
- Control 1 solo mueve Robot 1, y su LCD muestra "Control PAIR 1".
- Control 2 solo mueve Robot 2, y su LCD muestra "Control PAIR 2".
- Control 3 solo mueve Robot 3, y su LCD muestra "Control PAIR 3".
- No existe interferencia cruzada ni retrasos anormales.

## Parametros Tecnicos del Firmware

| Parametro | Valor | Ubicacion |
| :--- | :--- | :--- |
| Frecuencia de envio | 50 Hz (20ms) | control_tx.ino loop() |
| Timeout failsafe | 300 ms | robot_rx.ino FAILSAFE_TIMEOUT_MS |
| Reserva del contador NVS | 10 000 secuencias (~200 s) | control_tx.ino SEQUENCE_BLOCK_SIZE |
| Limite PWM inicial | 200 de 255 | robot_rx.ino MOTOR_MAX_DUTY |
| Resolucion PWM | 8 bits (0-255) | robot_rx.ino PWM_RESOLUTION_BITS |
| Frecuencia PWM | 5000 Hz | robot_rx.ino PWM_FREQUENCY_HZ |
| Deadzone joystick | 50 de 1000 | control_tx.ino JOYSTICK_DEADZONE |
| Promedio muestras ADC | 8 lecturas | control_tx.ino readAveraged() |
| Canal WiFi | Configurable | team_config.h WIFI_CHANNEL |
| Caducidad de confirmación | 300 ms | control_tx.ino STATUS_TIMEOUT_MS |
| Centros de calibración | 1536…2560 ADC | control_tx.ino CALIBRATION_CENTER_MIN/MAX |
| Variación máxima de captura | 100 ADC (máximo − mínimo) | control_tx.ino CALIBRATION_MAX_SPREAD |
| Captura de calibración | 128 lecturas por eje | control_tx.ino CALIBRATION_SAMPLES |
| Neutral previo al armado | 500 ms | robot_rx.ino ARM_NEUTRAL_MS |
| Estado periódico | 100 ms, más cambios de estado | robot_rx.ino STATUS_INTERVAL_MS |
| Actualización de motores | 10 ms | robot_rx.ino MOTOR_UPDATE_MS |
| Paso máximo de rampa | 40 unidades de mando | robot_rx.ino MOTOR_RAMP_STEP |
| Pausa a cero | 20 ms | robot_rx.ino MOTOR_REVERSE_PAUSE_MS |
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

- [ ] Failsafe verificado en los 3 robots (umbral de 300 ms y tiempo de frenado medido).
- [ ] Rango real probado en sede del evento.
- [ ] Consumo medido bajo carga durante 30 minutos de uso intenso.
- [ ] 2 juegos de baterias cargadas y balanceadas por robot.
- [ ] Repuestos disponibles: ESP32 extra, driver, cables, soldador.
- [ ] Switches y LEDs funcionales y visibles segun reglamento.
- [ ] Prueba de interferencia cruzada completada con 3 pares activos.
- [ ] Jumpers ENA/ENB retirados en todos los drivers L298N.
- [ ] Capacitores instalados en posiciones correctas.
- [ ] LED RGB de cada control y cada robot cambia de color correctamente (azul/amarillo/verde/rojo).
- [ ] LCD de cada control muestra el PAIR_ID correcto y responde a cambios de estado.
- [ ] Backpack de cada LCD confirmado a 3.3V, no a 5V.

## Verificación realizada sin compilar

Se revisaron las rutas de calibración, validación, armado, parada, failsafe, acceso compartido, respuesta de estado y error fatal. Se comprobaron diferencias y espacios con `git diff --check`.

La simulación reproducible `node tests/simulate_safety.cjs` usa únicamente Node.js, sin dependencias nuevas. Comprueba modelos de secuencias/reserva NVS con reinicios y desbordamiento, armado y cancelación, confirmaciones, límites de calibración y rampas con inversión y parada. Lee los parámetros del código para detectar diferencias de configuración. **Son simulaciones de lógica, no ejecución ni pruebas del firmware C++**, y no modelan Wi-Fi, FreeRTOS, ADC, NVS física o el puente H.

No se compiló ni se hicieron pruebas físicas. Quedan pendientes las pruebas manuales anteriores, especialmente compatibilidad completa de librerías, latencia real, calibración de cada joystick, frenado y comportamiento eléctrico.

## Mejoras Futuras

- Migracion a TB6612FNG para reducir perdidas por calor y mejorar eficiencia.
- Encoders + PID para correccion de trayectoria y control preciso.
- Ampliar el retorno de estado con medición de voltaje de batería y RSSI.
- Persistencia de lastSeq en robot para cerrar ventana de replay tras reinicio.
- Boton fisico de armado como requisito adicional de seguridad.

## Licencia

MIT License. Ver archivo LICENSE.

El software se provee "tal cual", sin garantias implicitas de idoneidad para combate, sabotaje activo o cumplimiento de reglamentos de competencia especificos. La seguridad implementada es adecuada para torneos locales sin adversarios con equipamiento de sniffing RF profesional.
