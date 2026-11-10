# Documentación del port ESP32-P4 — índice

Árbol: `/home/daniel/xinu-p4.2` · placa: ESP32-P4 (RV32 dual-core HP + core LP "ULP"),
16 MB flash QIO a 80 MHz, 32 MB PSRAM, ESP32-C6 en SDIO slot 1, microSD en slot 0,
ST7796 320x480 por SPI2, GT911 por I2C, AXP2101 (PMIC), ES8311 (codec audio).

Escrito el 2026-11-04 contra el árbol de la rama de trabajo. **Ningún `.c` ni `.h`
del kernel se ha modificado para escribir estos documentos** (las excepciones autorizadas
y justificadas abajo son las únicas líneas tocadas); todo lo afirmado se ha
leído del fuente o medido en placa, y cada afirmación lleva su `fichero:línea`.
Cuando algo es una **propuesta** (no está en el código) está marcado como tal.

Excepciones, autorizadas explícitamente el 2026-11-04: el comando `syscheck`
cableado en la shell (`shell/xsh_syscheck.c` + su fila en `shell/Makerules`,
`shell/shell.c` e `include/shell.h` — documento 06), el puerto del `httpd` completo
más su API REST (`shell/xsh_httpd.c` reescrito, `shell/httpapi.c` y `include/xsh_http.h`
nuevos), el comando `psram` (`shell/xsh_psram.c` nuevo, con sus filas en
`shell/Makerules`, `shell/shell.c` e `include/shell.h`) y el puerto del `udpserver`
(`shell/xsh_udpserver.c` nuevo con su fila en `shell/Makerules`, más las líneas de
`shell/shell.c`, `include/shell.h` y el lanzamiento no-fatal en `shell/xsh_netboot.c`) —
documento 03 §5 y §7, las credenciales de `netboot`
cambiadas a la AP del trabajo (`shell/xsh_netboot.c:55` — documento 03; re-cambiadas
el mismo día a la AP activa del domicilio, siempre solo en el fuente), el orden de
arranque de `system/main.c`: el dash se crea en linea (`main.c:71-78`, editado por Daniel a las
14:53) y la shell se crea **después** del mensaje de muerte de la hebra `netboot`
(`main.c:80-94`, gate por `receive()`), con la consola muda hasta que la red termina: medido
con el build #1548, `p4demo:` a los 1,8 s, `netboot:` a los 13,7 s (el
`sleep(NETBOOT_SETTLE_MS)` de la hebra), IP por DHCP a los 24,1 s y banner de la shell a los
24,2 s), y la reserva del coprocesador LP en
`machine/ulp/ulp.ld:48-49` con su pareja `ld.script:33`: `FLASH LENGTH = 64K` y
`RAM ORIGIN = 0x49FF8000, LENGTH = 32K`, con el `RAM LENGTH` del kernel subido a `32M-32K`
para que `_psram_free_end` pase de `0x49F00000` a `0x49FF8000`. Ganancia medida en placa:
heap PSRAM de 32.346.112 B a **33.361.920 B (+992 KB exactos)** (documento 07 §3.1).

## Cómo está repartido

| fichero | contiene | puntos que cubre |
|---|---|---|
| [01-memoria-nucleos.md](01-memoria-nucleos.md) | mapa de memoria real con cifras, SBL/ULP/kernel, cuánta RAM contar, regla de caché de PSRAM, core1 y core LP | 9, 20, 21 |
| [02-dispositivos.md](02-dispositivos.md) | audio (bring-up, formato, opcodes, quirks), power, LCD + framebuffer + LVGL, touch, SD/FAT | 1, 2, 8 |
| [03-red-y-wifi.md](03-red-y-wifi.md) | pila TCP/UDP/RAW, todos los comandos de red con ejemplos, segundo servidor HTML, `espeth`, `udpserver` + sesión de firmware, esp_hosted/WiFi | 3, 4, 5 |
| [04-elf-java-apps.md](04-elf-java-apps.md) | cargador ELF en vivo, librerías con `extern`, shell como busybox, `examples/p4-elfapp`, port de la JVM, catálogo de apps, Java vs ELF | 6, 7, 10, 11, 12 |
| [05-roadmap-y-conf.md](05-roadmap-y-conf.md) | TinySTT/TTS, TinyUSB, cámara OV5647, MP3, actualización por UDP + bootloader, dashboard en SD; y revisión de recursos de `conf.c` | 13..19 |
| [06-syscheck.md](06-syscheck.md) | cómo medir la velocidad real del CPU y si `millis()` funciona, y chequeo completo del kernel — **mandos cableados y cifras medidas** | (pedido inicial), 1 |
| [07-flash-map-diseño.md](07-flash-map-diseño.md) | **diseño, salvo la ULP ya recortada**: mapa de particiones de 16 MB, bootloader `bl2` detrás del SBL, A/B + `factory` + `stage`, y la ULP con **64 KB de flash y 32 KB de RAM** (aplicado en `ulp.ld`) | 18 |
| [08-smp.md](08-smp.md) | **diseño, nada implementado**: SMP real en los dos HP cores — L1-D compartida, L1-I por core, gran candado del kernel en `disable()/restore()`, planificador por core con afinidad, Timer por hart y fases de bring-up | (fase B) |

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
| **orden de arranque** | **red y panel antes que consola** (cambiado el 2026-11-04): `xsh$` es lo último | `system/main.c:80-101`; **validado en placa** el 2026-11-04 con el build #1555: el dash arranca a los 1,8 s, `netboot` cierra a los 21,5 s y el banner de la shell sale a los 21,6 s. `main` ya no se queda bloqueado de por vida: `ps` no lista el tid 5 (tareas #68, #71 y #72 cerradas) |
| telnet tras corte brusco | **no se reproduce** (2026-11-04: FIN y RST con `syscheck mem` imprimiendo en vuelo → el 23 vuelve a escuchar en 1 s, 4/4 ciclos) | tarea #62. El `Connection refused` es diseño: sin cola de *accept*, `-n 1` = un cliente (§6.6 de [03-red-y-wifi.md](03-red-y-wifi.md)). Quedan dos carreras heredadas del S3 sin tocar: `telnetWrite.c:50/:62` y `telnetFlush.c:31/:36` |
| audio TX + RX (micrófono) | operativo, validado al oído | anillos en `.audioshare`, quirk mono documentado |
| LCD + touch + power + audio en un panel | operativo | `p4demoTask` lanzado desde `system/main.c:73` |
| microSD + FAT | operativo | `ls`/`df`/`cat`; montaje y centinela en [02-dispositivos.md](02-dispositivos.md) |
| ELF en runtime (`elfapp`) | operativo para ET_REL RV32 | 235 relocaciones, selftest 26/27 |
| core1 | **aparcado**, no ejecuta nada | `README_CORE1.md` §1, testigo `0xC0DE1234` verificado |
| ULP | se flashea y se levanta, `main()` vacío | `machine/ulp/main.c:33` (bucle `while(1)` con el `printf` comentado) |
| httpd | **servidor completo**: estáticos desde la FAT + API REST de ficheros (`GET`/`PUT`/`DELETE`/`POST`), validado en placa el 2026-11-04 | `shell/xsh_httpd.c`, `shell/httpapi.c`; pruebas en [03-red-y-wifi.md](03-red-y-wifi.md) §5 |
| `udpserver` (comandos + subida de firmware por UDP) | **operativo y validado en placa el 2026-11-04**: puerto 5000, 11 comandos de estado y la sesión `FW_BEGIN/FW_DATA/FW_END/FW_STATUS/FW_ABORT` que apila la imagen en `/xinu.img` | `shell/xsh_udpserver.c`; 315.820 B subidos en 4,45 s y verificados byte a byte al bajarlos por el `httpd` → [03-red-y-wifi.md](03-red-y-wifi.md) §7 |
| aplicar esa imagen a la flash | **bloqueado por arquitectura** (sin driver de escritura/borrado de flash; el SBL no lee la SD): `FW_END` responde `apply=blocked` | [05-roadmap-y-conf.md](05-roadmap-y-conf.md) §6, tarea #58 |
| `psram` (ver el heap PSRAM) | operativo | `shell/xsh_psram.c` — rango, libre/usado, tabla de bloques con corte por lista pisada |
| cámara, USB device, MP3, STT/TTS, LVGL, Java | **no existe** en este árbol | [05-roadmap-y-conf.md](05-roadmap-y-conf.md) |

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
- **El `#NNNN` del banner no identifica la imagen.**  Xinu regenera `include/version.h` en
  cada `make` (`compile/mkvers.sh` incrementa `compile/vn`), pero el `Makefile:300` solo
  declara la dependencia de recompilación de `system/initialize.o`; `system/main.c`,
  `shell/xsh_lcd.c` y `shell/xsh_udpserver.c` incluyen `<version.h>` y conservan el valor del
  último `make` en que se recompilaron.  Medido en el kernel flasheado el 2026-11-04: el
  banner de arranque (`main.c:137`) dice `#1555`, el `VERSION` del udpserver
  (`xsh_udpserver.c:698`) dice `#1564` y la cabecera del dash LCD (`xsh_lcd.c:141`) llevaba
  `#1532` — los tres conviven en el mismo ELF (`strings compile/xinu.elf | grep '#15'`).  Al
  citar un número en un documento, dilo con su fuente («banner» o «`VERSION`») o identifica la
  imagen por la talla del kernel.
- Reglas del proyecto intactas (AGENTS.md §3): bare-metal sin ESP-IDF, nada de coma
  flotante en el kernel, etapas nuevas solo con autorización explícita.
