# Revisión previa a la competencia: Robot Soccer Amateur

Lista de lo que hay que revisar para cumplir el reglamento oficial de la categoría **3. Robot Soccer Amateur** (Feria de Robótica "Desafío robótico, creatividad en acción", Grupo Editorial Amazonas). Cada punto cita el artículo del reglamento.

El reglamento puede cambiar sin previo aviso (cap. 5) y el juez tiene la última palabra en caso de duda (art. 6.3). Revisar la versión vigente antes del evento.

**Leyenda:** `[x]` cumplido · `[~]` parcialmente o según el equipo, falta comprobar · `[ ]` pendiente

---

## 1. Estado general

| Artículo | Requisito | Estado |
| :--- | :--- | :--- |
| 1.1 / 4.1 | Equipo de 3 robots controlados de forma inalámbrica | `[x]` 3 pares ESP-NOW independientes |
| 1.2 / 2.4 | Motorreductor con caja amarilla | `[x]` Se usan motorreductores TT amarillos |
| 2.4 | Solo Arduino o ESP32 como microcontrolador | `[x]` ESP32 en robots y controles |
| 2.1 | Máximo 10 x 10 cm (o 10 cm de diámetro) | `[~]` Cumple según el equipo; medir cada robot montado |
| 2.1 | Sin mecanismo de pateo | `[x]` Frente en U fijo; el firmware solo mueve 2 motores |
| 2.2 | Tolerancia hasta 12 x 12 cm con penalización | `[ ]` Asegurar que no haga falta usarla |
| 2.3 | Zona de color de 3,5 x 3,5 cm en la parte superior | `[ ]` Pendiente de montar |
| 2.3 | No retener, succionar ni encajonar más del 30 % del balón | `[~]` Frente en U; comprobar con el balón oficial |
| 4.4 / 4.7 | Aguantar 2 x 5 min, alargue de 3 min y penales | `[ ]` Medir autonomía de la batería |
| 4.10 | Los 3 robots deben terminar el partido | `[ ]` Probar los 3 pares y preparar repuestos |

---

## 2. Revisión del robot (mesa de control técnico)

### 2.1 Motores (art. 1.2 y 2.4)

- [x] Los dos motores de cada robot son motorreductores TT con **caja amarilla**.
- [ ] Los 6 motores son del mismo modelo; no hay motores de otro tipo (N20, metálicos, etc.).
- [ ] Ruedas y ejes bien sujetos; ningún motor suelto ni con la caja rajada.
- [ ] Llevar motores de repuesto **también amarillos**.

### 2.2 Microcontrolador (art. 2.4)

- [x] Robots con ESP32 DevKit.
- [x] Controles con ESP32 (DevKit en `control_tx`, CYD en `control_cyd`).
- [ ] Ninguna otra placa programable en el robot (el L298N, el buck y el BMS no son microcontroladores).

### 2.3 Dimensiones (art. 2.1 y 2.2)

- [ ] Medir cada robot **completamente montado** (con ruedas, cables, batería y tapa) con regla o calibrador.
- [ ] Largo y ancho **≤ 10 cm** cada uno. Si es cilíndrico, diámetro ≤ 10 cm.
- [ ] Nada sobresale de la planta: cables, interruptor, conectores, tornillos, antena del ESP32.
- [ ] Las ruedas cuentan en la medida: medir de borde exterior a borde exterior.
- [ ] Hacer una plantilla o caja de **10 x 10 cm** (cartón o impresión 3D) y comprobar que cada robot entra en ella. La mesa técnica suele medir así.

> **Penalización:** entre 10 y 12 cm se permite, pero da **1 gol al rival** o una sanción técnica. Más de 12 cm **descalifica**. La meta es no usar la tolerancia.

### 2.4 Sin mecanismo de pateo (art. 2.1)

- [x] El robot no tiene solenoide, resorte, servo ni ninguna pieza móvil para golpear el balón.
- [ ] La U delantera está **fija**: no tiene bisagras ni partes que se muevan al chocar.
- [ ] No hay piezas elásticas que funcionen como catapulta.

### 2.5 Zona de color (art. 2.3)

Es una superficie plana de un solo color, de **3,5 x 3,5 cm como mínimo**, en la cara superior, para que los jueces identifiquen el robot desde arriba.

- [ ] Preguntar a la organización qué color usar o si lo eligen ellos (el reglamento no lo dice).
- [ ] Cuadrado de **3,5 x 3,5 cm o más** en la tapa de los 3 robots.
- [ ] Material firme y mate: vinilo adhesivo, cartulina plastificada, acrílico de color o pintura. Que no se despegue con los choques.
- [ ] Zona **despejada**: sin cables, tornillos, LED ni componentes encima o tapándola.
- [ ] Se ve desde arriba con el robot en el suelo (no queda tapada por una pieza más alta).
- [ ] Si se usa una placa elevada, no aumenta la planta de 10 x 10 cm.
- [ ] Llevar repuestos del material de la zona de color.

> El LED RGB de estado **no sustituye** a esta zona: es un punto, no una superficie de 3,5 x 3,5 cm, y cambia de color.

### 2.6 Retención del balón (art. 2.3)

El robot no puede retener, succionar ni encajonar el balón en más del 30 % de su estructura.

- [ ] Conseguir el **balón oficial** o uno del mismo tamaño y medir su diámetro.
- [ ] Apoyar el balón al fondo de la U: como mucho un **30 %** del balón debe quedar dentro. Referencia: profundidad de la U ≤ 0,3 x diámetro del balón.
- [ ] La U no se cierra alrededor del balón ni tiene "labios" que lo atrapen.
- [ ] Al retroceder, el robot **suelta** el balón (no lo arrastra hacia atrás).
- [ ] Sin ventiladores, imanes ni superficies adhesivas.
- [ ] En caso de duda, mostrar la U a los jueces antes del primer partido.

---

## 3. Funcionamiento durante el partido

### 3.1 Control inalámbrico (art. 1.1)

- [ ] Los 3 pares calibran, se arman y se mueven. Probar con el robot en el suelo, no solo con las ruedas levantadas.
- [ ] Cada control mueve **solo su** robot, con los 3 encendidos a la vez.
- [ ] Probar el alcance en la cancha: 1,85 x 1,34 m, controles a un costado.
- [ ] Probar en la sede, con el WiFi del lugar y los robots del equipo rival encendidos. Si hay pérdidas (LED rojo, rearmados frecuentes), cambiar `WIFI_CHANNEL` en los `team_config.h` (control y robot del mismo par iguales) y volver a cargar.
- [ ] Llevar los `team_config.h` respaldados (fuera del repositorio) para poder recargar una placa en el evento.

### 3.2 Duración del partido y batería (art. 4.4 y 4.7)

Un partido puede durar: 2 x 5 min + 2 min de descanso + 2 min + 3 min de alargue + penales.

- [ ] Medir la autonomía de cada robot con juego continuo: **al menos 15 min** sin bajar de velocidad ni reiniciarse.
- [ ] Batería de cambio cargada por robot, con conector rápido (JST o XT30) para cambiarla en el **descanso de 2 minutos**.
- [ ] Medir el voltaje con multímetro antes de cada partido (el firmware no avisa de batería baja). Una 2S cargada marca 8,4 V; por debajo de unos 7,0 V conviene cambiarla.
- [ ] Controles: CYD y ESP32 con batería o power bank cargados para todo el partido.
- [ ] Cargador 2S de balance (8,4 V) disponible en el evento.

### 3.3 Sin cambios de robot (art. 4.10)

Los tres robots que empiezan deben terminar. Si dos se averían, se pierde por 2 goles.

- [ ] Fijar bien todos los cables (bridas, termorretráctil, conectores con traba).
- [ ] Batería sujeta: que no se suelte con un choque.
- [ ] Capacitor de 470–1000 µF instalado en cada driver (evita reinicios por picos).
- [ ] Probar choques fuertes contra la pared y entre robots: el robot no se reinicia. Si se reinicia, revisar la alimentación del ESP32.
- [ ] Kit de reparación rápida: ESP32 de repuesto ya configurado, L298N, motores amarillos, ruedas, cables, cinta, bridas, destornillador, multímetro, soldador.
- [ ] Practicar el rearmado: tras un failsafe o un reinicio el robot queda Desarmado y hay que repetir el gesto (centro 500 ms, pulsar y soltar).

### 3.4 Roles y zonas (art. 4.2 y 4.3)

Esto lo controla el piloto, no el firmware.

- [ ] Decidir qué robot es el **arquero** (solo uno a la vez). Se queda en su zona de 30 cm.
- [ ] El arquero **no retiene** el balón en las esquinas.
- [ ] Los dos jugadores disputan el balón en la zona central (1 m); pueden entrar a la zona del arquero rival para hacer gol.
- [ ] Practicar el saque: al iniciar desde el centro, el equipo atacante mueve el balón hacia su propio campo (art. 4.5).
- [ ] Practicar penales desde media cancha sin portero (art. 4.8). En la primera ronda **cada robot tira uno**, sin repetir (art. 4.9): los 3 deben poder tirar.
- [ ] Recordar el cambio de lado en el segundo tiempo (art. 4.4).

### 3.5 Manejo con los controles

- [ ] **Control de joystick:** el botón se activa hundiendo la palanca. Si se aprieta sin querer, el robot frena y se desarma. Practicar la conducción sin hundir la palanca.
- [ ] **Control CYD:** el táctil es de un solo toque; no se puede mover el joystick y pulsar ARMAR/FRENO a la vez. Al levantar el dedo el robot se detiene.
- [ ] Calibrar cada control con el joystick suelto (o la pantalla sin tocar) antes de cada partido.
- [ ] Ajustar la velocidad si hace falta: `MOTOR_MAX_DUTY` en `robot_rx` (200 de 255 por defecto). Medir el voltaje en el motor antes de subirlo (los TT son de 3–6 V).

---

## 4. Checklist del día de la competencia

**Antes de la mesa técnica**

- [ ] Cada robot entra en la plantilla de 10 x 10 cm.
- [ ] Zona de color de 3,5 x 3,5 cm visible en los 3 robots.
- [ ] Motores amarillos a la vista y en buen estado.
- [ ] U delantera fija, sin encajonar más del 30 % del balón.
- [ ] Ninguna pieza de pateo ni partes móviles.

**Antes de cada partido**

- [ ] Baterías cargadas (multímetro) y de repuesto listas.
- [ ] Encender los 3 pares: cada control calibra y muestra Desarmado.
- [ ] Armar cada robot y probar avance, retroceso y giros.
- [ ] Apagar un control y comprobar que su robot frena (failsafe).
- [ ] Decidir el arquero y el orden de penales.

**En el descanso (2 minutos)**

- [ ] Cambiar baterías si bajaron.
- [ ] Revisar cables y ruedas.
- [ ] Cambiar de lado de cancha.

**Después del partido**

- [ ] Apagar los robots y revisar motores, ruedas, cables y driver.
- [ ] Anotar cualquier reinicio, pérdida de señal o fallo para corregirlo antes del siguiente.

---

## 5. Preguntas para la organización

- [ ] ¿Qué color debe tener la zona de 3,5 x 3,5 cm? ¿Es por equipo o por robot?
- [ ] ¿Qué balón se usa (tamaño y material)?
- [ ] ¿Cómo se mide el 30 % de retención del balón?
- [ ] ¿Hay restricción de canales WiFi o de radio en la sede?
- [ ] ¿Se puede cambiar la batería durante el descanso? (El reglamento permite "ajustes" en el descanso de 2 minutos.)
- [ ] ¿Se puede reemplazar una pieza averiada (no el robot entero) durante el partido?
