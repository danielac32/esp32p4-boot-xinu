# WiFi sobre el ESP32-C6 (esp_hosted) en el slot SDIO 1 del P4

Estado de la fase W2 del port, qué está medido en placa, qué hipótesis quedaron
descartadas y dónde sigue el bloqueo. Este documento es el punto de partida para
retomar sin repetir experimentos ya hechos.

Placa: Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 (ESP32-P4 + ESP32-C6 como
coprocesador WiFi corriendo firmware slave de `esp_hosted`).
Fuente de referencia del port: `/home/daniel/esp32-riscv-bare-metal-sdk-main`
(`drivers/esp_hosted`, `drivers/lwip_hosted`, `core/soc/esp32p4`,
`core/hal/esp32p4`). **Regla del árbol: ESP-IDF no es fuente**; todo registro y
cabecera sale del SDK bare-metal.

---

## 1. Cómo se construye, se flashea y se prueba

```sh
cd /home/daniel/xinu-p4.2/compile && make
make ESPTOOL=/home/daniel/esp-idf-py/bin/esptool flashxinu
/home/daniel/esp-idf-py/bin/python /tmp/p4time.py "wifi fn1" "ls"
```

`/tmp/p4time.py` pulsa el reset (RTS), espera `xsh$` y pasa comandos por
`/dev/ttyACM0` (115200, uso exclusivo). Transcripciones de consola de esta fase:
`/tmp/p4fn1.log` (escalera vieja), `/tmp/p4fn1b.log` (reloj coherente) y
`/tmp/p4fn1c.log` (prueba de escritura CMD53).

Comandos de shell de la sonda: `wifi probe` (W1, solo enumeración), `wifi fn1`
(W2, transporte de la función 1) y `wifi diag` (volcado de registros).

---

## 2. Arquitectura del bloque: un SDHOST, dos slots

El ESP32-P4 tiene **un** controlador SDHOST (`0x50083000`) con **dos slots** que
comparten motor de comandos, IDMAC, FIFO, `STATUS` y `INTMASK`:

| | slot 0 | slot 1 |
|---|---|---|
| dispositivo | microSD | ESP32-C6 (esp_hosted slave) |
| pads | fijos, 39..44 (`mcu_sel=0`) | matriz GPIO: CLK 18, CMD 19, D0 14, D1 15, D2 16, D3 17, reset 54 (activo bajo) |
| reloj | `CLKDIV.divider0`, `CLKSRC.card0=0` | `CLKDIV.divider1`, `CLKSRC.card1=1` |
| ancho | `CTYPE.card_width` bit 0 | `CTYPE.card_width` bit 1 |

Por slot: `CLKDIV` (un byte cada uno, divisor = `2*n`, `n=0` = salto), `CLKSRC`
(2 bits por slot), `CLKENA` (bit `slot` y bit `16+slot` para low-power) y
`STATUS`/`CMD.card_num`. Compartidos: `CMD`, `CMDARG`, `TMOUT`, `BLKSIZ`,
`BYTCNT`, `CTRL`, `BMOD`, `IDSTS`, `DBADDR`, `FIFO`, `STATUS`.

Dos cosas que costaron tiempo y que ya están fijadas con evidencia:

* **`CLKSRC` no selecciona "salidas del divisor global"**: enlaza cada slot con
  una de las cuatro divisiones internas del bloque (`soc/sdmmc_struct.h`,
  `sdhost_clksrc_reg_t`). Poner `card1=0` enlaza el C6 con la división de la
  microSD. El SDK hace lo mismo en `sdmmc_ll_set_card_clock_div`
  (`core/hal/esp32p4/include/hal/sdmmc_ll.h`): `clksrc.card1 = 1`.
* **`card_num` vive en el registro CMD** (`[20:16]`, "physical slot number of
  card being accessed") y es lo que enruta la fase de datos al slot. Se pone en
  `sdTransferSlot()` con `SD_CMD_CARD_NUM_POS`.

Reloj global del bloque: `HP_SYS_CLKRST.peri_clk_ctrl02` (`reg_sdio_ls_clk_edge_*`,
4 bits cada campo → divisor válido 1..16 → mínimo 160/16 = 10 MHz). Ahora es
programable en tiempo de ejecución: `sdSetHostDiv()` en
[device/sdmmc-esp32p4/sdHw.c:481](file:///home/daniel/xinu-p4.2/device/sdmmc-esp32p4/sdHw.c#L481),
y `sd_host_khz` informa de él.

### Enquistamiento compartido (medido)

Un CMD53 cuya fase de datos muere deja `STATUS.data_state_mc_busy=1`, y con ese
bit arriba el "update clock" queda **inhibido para los dos slots**: la microSD no
puede ni cambiar de reloj y `ls` devuelve el directorio vacío hasta reiniciar.
Ni `sdControllerInit()` (solo resetea si los bits de `CTRL` vienen ya alzados) ni
los reseteos parciales de `sdDataAbort()` lo despejan. La única salida es un
reset forzado `CTRL = CONTROLLER|FIFO|DMA`: `sdBlockReset()` en
[device/sdmmc-esp32p4/sdHw.c:590](file:///home/daniel/xinu-p4.2/device/sdmmc-esp32p4/sdHw.c#L590).

---

## 3. Qué está funcionando (con valor leído del periférico)

**W1, enumeración del C6 en el slot 1** — completa y estable: CMD0+CMD5 (OCR
`R4[31]` interfaz lista), CMD3 (RCA), CMD7, selección de la función 1 y lectura
de la región de identificación del CCCR. El reloj del slot se pone **dentro de la
única ventana legal** (tras el reset del C6 y antes del CMD5, con DAT0 aún
libre): `sdioEnumerateAt()` en
[device/sdio-esp32p4/sdioProbe.c:389](file:///home/daniel/xinu-p4.2/device/sdio-esp32p4/sdioProbe.c#L389).

**Canal de control de la función 1** — intacto en la pasada de transporte:

* Puesta a punto CCCR por CMD52: `bus=0x42` (leído del slave, 4-bit),
  `blksize fn0=0x0200 fn1=0x0200`, `fallos=0`; `CCCR_INT_ENABLE=0x03` (maestro +
  fn1) armada.
* Nudge `HOST_TO_SLAVE` (CMD52 write, BIT(0)) aceptado.
* El esclavo **anuncia un paquete C2H real**: `INT_RAW=0x00cc4000`
  (`new_packet=1`), `PACKET_LEN=0x0000001a` (26 bytes),
  `TOKEN_RDATA=0x00141000` (token=20). Esos valores salen del C6, no de caché.
* Con el C6 retenido en reset (`sdioC6HoldReset()`,
  [sdioProbe.c:233](file:///home/daniel/xinu-p4.2/device/sdio-esp32p4/sdioProbe.c#L233))
  `STATUS.data_busy` pasa de 1 a 0: el bit sigue de verdad a la línea DAT0 del
  esclavo, así que **la entrada DAT0 del slot 1 conduce**.

**Coexistencia con la microSD** — la sonda termina SIEMPRE devolviendo el bloque
al estado de arranque: retiene el C6, `sdBlockReset()`, devuelve el divisor
global a `SD_HOST_DIV`, `sdControllerInit()` + `sdIdentify()` + lectura real del
LBA0 y comprobación de firma. Medido en las tres pasadas de hoy:
`ctrl=1 ident=1 lectura LBA0=1 firma=0x55aa -> microSD VIVA` y `ls` lista
`txt.txt bin/ config/`. R1..R4 (base, dash, psram, FAT) siguen verdes.

---

## 4. El bloqueo: la fase de datos del CMD53

Ninguna transacción CMD53 mueve un byte, en ninguna configuración probada:

| pasada | ancho | reloj slot 1 | `sd_hwerr` | `STATUS` | FIFO |
|---|---|---|---|---|---|
| B | 4-bit | 20 MHz (`card_div=1`) | `0x3000a000` | `0x0001ad06` | 0 bytes |
| B | 1-bit | 20 MHz | `0x4000a000` | `0x0001ab06` | 0 bytes |
| C | 4-bit | 22,857 MHz (`global=7`, `card_div=0`) | `0x3000a000` | `0x0001ad06` | 0 bytes |
| C | 1-bit | 22,857 MHz | `0x4000e000` | `0x0001ab06` | 0 bytes |
| C | 4-bit | 10 MHz (`global=16`) | `0x3000a000` | `0x0001ad06` | 0 bytes |
| C | 1-bit | 10 MHz | `0x4000a000` | `0x0001ab06` | 0 bytes |
| C | **escritura** 4 B al doorbell | 22,857 MHz | `0x30000010` | — | — |

Lectura de la tabla:

* `0x?000a000` = `SD_IRQ_SBE | SD_IRQ_EBE` (start-bit y end-bit error) con
  `resp_idx=53`, `fifo_count=0` y `data_state_mc_busy=1`: el CMD53 **recibió
  respuesta** (el esclavo aceptó el comando) pero la línea DAT nunca presentó un
  bloque de datos válido.
* La etiqueta `timeout (silencio del slave)` engaña: con `CMD.wait_complete=1`
  el `CMD_DONE` solo llega al terminar el dato, así que el bucle de comando
  agota su tiempo viendo `SBE|EBE` en `RINTSTS`. Es un error de dato, no un
  comando ignorado.
* **La escritura H2C también falla, y de otra forma**: `0x30000010` es
  `SD_IRQ_TXDR` (petición de hueco en el FIFO de transmisión) sin `CMD_DONE`,
  es decir el **IDMAC no alimentó el FIFO** con los 4 bytes que ya estaban en la
  rebote. Eso es un problema del camino host→memoria, independiente del C6.

### Descartado con medición, no volver a probar

1. **Índices de señal de la matriz GPIO del slot 1.** `SDIO_SIG_CLK..D3 = 0..5`
   ([sdioProbe.c:43](file:///home/daniel/xinu-p4.2/device/sdio-esp32p4/sdioProbe.c#L43))
   coinciden con `soc/gpio_sig_map.h` del SDK (`SD_CARD_CCLK_2=0`, `CCMD_2=1`,
   `CDATA0_2..CDATA3_2=2..5`, y `slot_gpio_sig[1]` en `core/soc/esp32p4/sdmmc_periph.c`).
   El CLK no tiene `PAD_IN` y `sdioRoute()` no lo enruta como entrada: correcto.
2. **Estructura del reloj de transferencia.** El reference jamás mueve datos con
   `card_div != 0`; usa `host_div=7` + `card_div=0` (22,857 MHz). Repetido tal
   cual: mismo fallo. Y a 10 MHz también. **No es muestreo ni velocidad.**
3. **Negociación de ancho.** Se comprueba en los dos lados: CCCR 0x07 leído del
   slave (`0x42` para 4-bit, `0x40` para 1-bit) y bit 1 de `CTYPE`. La pasada de
   1-bit deja `CTYPE=0x00000001` (verificado en el volcado del fallo), así que
   host y esclavo estaban de acuerdo.
4. **Que el esclavo no conteste a CMD53.** `resp_idx=53` en `STATUS` y los
   registros de la función 1 respondiendo a CMD52 durante toda la pasada.
5. **Camino DMA/IDMAC general** (bit `CACHE_L2_CACHE_SHUT_DMA`): con él cerrado
   por biseción, el slot 0 sigue leyendo bloques por DMA (arranque, FAT y el
   LBA0 de la comprobación final). No es la caché L2.
6. **Que `sdControllerInit()` bastara para desclavar el motor**: no bastaba; de
   ahí `sdBlockReset()` y la verificación con firma del LBA0.

### Hipótesis viva y siguiente medición

La evidencia nueva de hoy (escritura con `TXDR` sin `CMD_DONE`) dice que el
**IDMAC no está tomando el descriptor** en el slot 1, y eso explicaría también la
lectura: si el FIFO no se drena, el CIU no puede sostener la fase de datos.
Falta la medición directa, barata:

* Volcar tras un CMD53 fallido: `CTRL`, `BMOD`, `IDINTEN`, `IDSTS` (y su FSM),
  `DBADDR`, `RINTSTS`, `FIFOTH` y **el campo `OWN` del descriptor en RAM** (si
  sigue en 1, el IDMAC nunca lo tocó).
* Con eso se decide entre dos ramas: (a) fetch del descriptor (rearmar
  `PLDMND`, revisar `BMOD.SW_RESET`/`ARB_BURST_LEN`, o el `FIFOTH`/`MSIZE`), o
  (b) el CIU nunca pide el dato porque el slot 1 necesita algo que el slot 0 ya
  trae de serie (`TMOUT` de dato, `HCON`, umbral de SDIO en `INTMASK[17:16]`).

Ramas alternativas, por si (a) se descarta:

* **Flashear el ejemplo del SDK** (`examples/esp_hosted`, ya acordado como plan B):
  si su host mueve datos en esta placa, queda la secuencia exacta como
  referencia; si falla igual, el problema no es de este port.
* Comprobar el **modo SDIO del esclavo** (`packet` vs `streaming`, que el
  ejemplo imprime tras el RPC de capacidades): en modo streaming la ventana
  `0x1F800-len` no significa lo mismo y el C6 no conduciría DAT.

---

## 5. Ficheros de esta fase

| Fichero | Qué es |
|---|---|
| [device/sdmmc-esp32p4/sdHw.c](file:///home/daniel/xinu-p4.2/device/sdmmc-esp32p4/sdHw.c) | motor del slot 0 generalizado a dos slots: `sdTransferSlot` (línea 1085), `sdBlockReset` (590), `sdSetHostDiv` (481), `sdBusState` (443) |
| [device/sdmmc-esp32p4/sdhost.h](file:///home/daniel/xinu-p4.2/device/sdmmc-esp32p4/sdhost.h) | mapa de registros del SDHOST, masks de error `SD_IRQ_*`/`SD_IDSTS_*`, plan de reloj |
| [device/sdio-esp32p4/sdioProbe.c](file:///home/daniel/xinu-p4.2/device/sdio-esp32p4/sdioProbe.c) | W1: pads/routeo a la matriz, reset del C6, enumeración en la ventana legal |
| [device/sdio-esp32p4/sdioFn1.c](file:///home/daniel/xinu-p4.2/device/sdio-esp32p4/sdioFn1.c) | W2: sonda de transporte de la función 1 (`fn1ProbeOnce` 558,CMD53 171, envoltorio con escalera de relojes 1033, restauración del slot 0) |
| [device/sdio-esp32p4/espHostedSdio.c](file:///home/daniel/xinu-p4.2/device/sdio-esp32p4/espHostedSdio.c) | port verbatim de `esp_hosted_sdio.c` (attach, RPC, cola C2H/H2C) — aún no es el camino que ejercita `wifi fn1` |
| [include/sdio.h](file:///home/daniel/xinu-p4.2/include/sdio.h) | interfaz pública de la sonda |
| [shell/xsh_wifi.c](file:///home/daniel/xinu-p4.2/shell/xsh_wifi.c) | comandos `wifi probe / fn1 / diag` |

Fases siguientes, en orden: W2 (esto) → W3 lwIP sobre `lwip_hosted` (ojo:
`third_party/lwip` del SDK está **vacío**; hay que decidir fuente antes de
escribir) → W4 conectar/DHCP → W5 aplicación que demuestre conectividad.

---

## 6. Notas de campo

* La tarea `p4demoTask` que sustituye al dash en `system/main.c` es del dueño del
  árbol: no quitar ni reordenar.
* Tras cualquier prueba de WiFi hay que correr `ls` y confirmar que la microSD
  sigue viva; la propia sonda ya lo imprime (`firma=0x55aa -> microSD VIVA`).
* Los valores que se reportan por consola salen del periférico o del slave, nunca
  de una variable cacheada. (Se corrigió el informe de ancho, que imprimía
  `sd_host_width` — el de la microSD — y mentía en la pasada de 1-bit.)
