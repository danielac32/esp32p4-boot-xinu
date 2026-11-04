# Documentación del port ESP32-P4 — índice

Árbol: `/home/daniel/xinu-p4.2` · placa: ESP32-P4 (RV32 dual-core HP + core LP "ULP"),
16 MB flash QIO a 80 MHz, 32 MB PSRAM, ESP32-C6 en SDIO slot 1, microSD en slot 0,
ST7796 320x480 por SPI2, GT911 por I2C, AXP2101 (PMIC), ES8311 (codec audio).

Escrito el 2026-11-04 contra el árbol de la rama de trabajo. **Ningún `.c` ni `.h`
del kernel se ha modificado para escribir estos documentos** (las tres excepciones autorizadas
y justificadas abajo son las únicas líneas tocadas); todo lo afirmado se ha
leído del fuente o medido en placa, y cada afirmación lleva su `fichero:línea`.
Cuando algo es una **propuesta** (no está en el código) está marcado como tal.

Excepciones, las tres autorizadas explícitamente el 2026-11-04: el comando `syscheck`
cableado en la shell (`shell/xsh_syscheck.c` + su fila en `shell/Makerules`,
`shell/shell.c` e `include/shell.h` — documento 06), las credenciales de `netboot`
cambiadas a la AP del trabajo (`shell/xsh_netboot.c:55` — documento 03), y el orden de
arranque de `system/main.c`: la shell se crea **después** del mensaje de muerte de la
hebra `netboot` (`main.c:79-99`, gate por `receive()`), con la consola muda hasta que
la red termina: medido con el build #1542, el `xsh$` sale a los **20,9 s** del reset
(`netboot:` a los 12,5 s — el `sleep(NETBOOT_SETTLE_MS)` de la hebra —, IP por DHCP a
los 20,3 s y banner de la shell a los 20,4 s).

## Cómo está repartido

| fichero | contiene | puntos que cubre |
|---|---|---|
| [01-memoria-nucleos.md](01-memoria-nucleos.md) | mapa de memoria real con cifras, SBL/ULP/kernel, cuánta RAM contar, regla de caché de PSRAM, core1 y core LP | 9, 20, 21 |
| [02-dispositivos.md](02-dispositivos.md) | audio (bring-up, formato, opcodes, quirks), power, LCD + framebuffer + LVGL, touch, SD/FAT | 1, 2, 8 |
| [03-red-y-wifi.md](03-red-y-wifi.md) | pila TCP/UDP/RAW, todos los comandos de red con ejemplos, segundo servidor HTML, `espeth`, esp_hosted/WiFi | 3, 4, 5 |
| [04-elf-java-apps.md](04-elf-java-apps.md) | cargador ELF en vivo, librerías con `extern`, shell como busybox, `examples/p4-elfapp`, port de la JVM, catálogo de apps, Java vs ELF | 6, 7, 10, 11, 12 |
| [05-roadmap-y-conf.md](05-roadmap-y-conf.md) | TinySTT/TTS, TinyUSB, cámara OV5647, MP3, actualización por UDP + bootloader, dashboard en SD; y revisión de recursos de `conf.c` | 13..19 |
| [06-syscheck.md](06-syscheck.md) | cómo medir la velocidad real del CPU y si `millis()` funciona, y chequeo completo del kernel — **mandos cableados y cifras medidas** | (pedido inicial), 1 |
| [07-flash-map-diseño.md](07-flash-map-diseño.md) | **diseño, nada aplicado**: mapa de particiones de 16 MB, bootloader `bl2` detrás del SBL, A/B + `factory` + `stage`, y la ULP recortada a 64 KB de flash y 20 KB de RAM | 18 |

Dashboard Material Design de todo esto:
[xinu-p4-dashboard.html](../xinu-p4-dashboard.html) (ver §"dashboard" de
[05-roadmap-y-conf.md](05-roadmap-y-conf.md) para servirlo desde la placa).

## Estado de lo que hay en placa hoy (medido, no deducido)

| cosa | estado | evidencia |
|---|---|---|
| WiFi asociado + DHCP + tráfico | operativo | `netboot` → `172.16.0.117` en la AP del trabajo (`PhMNet_2`), `192.168.1.146` en la de casa; 200 ping sin pérdida tras el arreglo de la cola RX |
| **velocidad real del core** | **319,977 MHz**, no los 360 declarados | `syscheck cpu` (`mcycle/mtime` = 0,9999) + `p4clk.py` (320,010 MHz contra el PC) → [06-syscheck.md](06-syscheck.md) §1 |
| **`millis()`** | **funciona de forma, falla en valor**: monótona y sin ticks perdidos, pero **1 s del kernel = 1,26 s reales** | `syscheck clk` (402 818 cuentas/ms en vez de 360 000), ratio kernel/PC 0,78066 → [06-syscheck.md](06-syscheck.md) §3 |
| comprobación del kernel | **23 tests, 19 OK, 4 avisos, 0 fallos** | `syscheck all` desde la shell → [06-syscheck.md](06-syscheck.md) §4 |
| RSSI | fiable bajo estrés | RPC 257 vía `esp_hosted_ctrl_raw_request()` ([03-red-y-wifi.md](03-red-y-wifi.md#wifi)) |
| latencia en reposo | **pendiente** (p50 ≈ 81 ms) | power save del C6, sin API en el SDK bare-metal → tarea #61 |
| **orden de arranque** | **red antes que consola** (cambiado el 2026-11-04): `xsh$` aparece tras el resumen de `netboot` | `system/main.c:79-99`; **validado en placa** el 2026-11-04 con el build #1542: un solo `xsh$` en toda la salida, a los 20,9 s del reset, justo antes de `p4demo: INICIO de tarea` (tarea #68 cerrada) |
| telnet tras corte brusco | **no se reproduce** (2026-11-04: FIN y RST con `syscheck mem` imprimiendo en vuelo → el 23 vuelve a escuchar en 1 s, 4/4 ciclos) | tarea #62. El `Connection refused` es diseño: sin cola de *accept*, `-n 1` = un cliente (§6.6 de [03-red-y-wifi.md](03-red-y-wifi.md)). Quedan dos carreras heredadas del S3 sin tocar: `telnetWrite.c:50/:62` y `telnetFlush.c:31/:36` |
| audio TX + RX (micrófono) | operativo, validado al oído | anillos en `.audioshare`, quirk mono documentado |
| LCD + touch + power + audio en un panel | operativo | `p4demoTask` lanzado desde `system/main.c:117` |
| microSD + FAT | operativo | `ls`/`df`/`cat`; montaje y centinela en [02-dispositivos.md](02-dispositivos.md) |
| ELF en runtime (`elfapp`) | operativo para ET_REL RV32 | 235 relocaciones, selftest 26/27 |
| core1 | **aparcado**, no ejecuta nada | `README_CORE1.md` §1, testigo `0xC0DE1234` verificado |
| ULP | se flashea y se levanta, `main()` vacío | `machine/ulp/main.c:33` (bucle `while(1)` con el `printf` comentado) |
| httpd | mínimo, un `index` embebido | `shell/xsh_httpd.c:45` |
| UDP server, cámara, USB device, MP3, STT/TTS, LVGL, Java | **no existe** en este árbol | [05-roadmap-y-conf.md](05-roadmap-y-conf.md) |

## Convenciones de este documento

- `xsh$` es la consola del P4 (UART0 a 115200 por el USB-C; en este PC, `/dev/ttyACM0`).
- Abrir el puerto **resetea la placa** (RTS/DTR del puente USB-serial); para hablar con
  una placa ya arrancada usa telnet o los scripts `/tmp/p4log.py` (con reset) y
  `/tmp/p4tel.py` (sin reset).
- **La IP es de DHCP y cambia con la AP**: `172.16.0.117` en la del trabajo
  (`PhMNet_2`), `192.168.1.146` en la de casa.  Los ejemplos escritos con una de
  las dos hay que leerlos con la que toque; lo que dice la salida no depende de
  la dirección.  La IP real la dicen `ifconfig` (la línea `inet`), el resumen de
  `netboot` (`netboot: ETH0 es …`) o `netstat`.
- Los scripts PC-side que abren el puerto serie van con
  `/home/daniel/esp-idf-py/bin/python` (el `python3` del sistema no tiene
  `pyserial`); `p4clk.py` no abre el puerto, usa telnet.
- "medido en placa" = hay una salida copiada en el documento. "propuesto" = nadie lo
  ha ejecutado todavía; normalmente cuesta una etapa y tu autorización.
- Reglas del proyecto intactas (AGENTS.md §3): bare-metal sin ESP-IDF, nada de coma
  flotante en el kernel, etapas nuevas solo con autorización explícita.
