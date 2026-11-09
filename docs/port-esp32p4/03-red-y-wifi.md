# 03 · Red: pila nativa, comandos, servidores y WiFi

---

## 1. Quién es quién

```
  aplicación      xsh_ping  xsh_telnet  xsh_httpd  p4demoTask   (tu código)
                          │           │         │
  device Xinu   TELNET0..3 (28-31)  TCP0..9 (18-27)  UDP0/1 (16-17)  RAW0/1 (14-15)
                          device/tcp/*.c     device/udp  device/raw
                                     │            │           │
  pila           network/ip/  ipInput/ipOutput  arp.c  route.c  icmp.c
                          │
  netif          network/net/netif.c · netInit.c · netUp.c · netDown.c · netRecv.c
                          │  (hebra `netRecv` por interfaz, prioridad 30)
  driver         device/espeth-esp32p4/espeth*.c      <-- ETH0, ranura 13
                          │
  transporte     device/sdio-esp32p4/espHosted*.c  (RPC + colas de tramas Ethernet)
                          │
  bus            device/sdmmc-esp32p4/sdHw.c  SDHOST slot 1  <-- ESP32-C6
```

Ranuras de `include/conf.h:34-66`. No hay **ninguna** interfaz Ethernet física: `ETH0` es
el nombre interno del `netif` que mueve tramas WiFi a través del C6. Se llama así porque
Xinu pide que cada `netif` sea un device de tipo "ether-like"; el tipo real se llama
`espeth`.

---

## 2. `espeth-esp32p4`: qué es y por qué "no está en conf.c" (punto 4)

Es el **driver de interfaz de red** — el equivalente a `de/Ethernet` de otros puertos, pero
sobre SDIO. Ficheros: `device/espeth-esp32p4/{espeth,espethInit,espethOpen,espethClose,
espethRead,espethWrite,espethControl}.c`.

- `espethRead()` saca tramas Ethernet del anillo que llena `esp_hosted_poll()`.
- `espethWrite()` mete tramas Ethernet hacia el C6.
- `espethControl()` (opcodes en `include/espeth.h:50-52`):
  `260 LINK` (¿está asociado el STA?), `261 MAC_SET`, `262 STATS` (rellena
  `struct espeth_stats {rx_frames, rx_drops, tx_frames, tx_drops}`).

**Sí está en `conf.c`** — lo que pasa es que no lo buscaste con ese nombre. En el generado:
[system/conf.c:133-138](../../system/conf.c#L133-L138) es la fila del `devtab` con
`espethInit/espethOpen/espethClose/espethRead/espethWrite/espethControl`, y la macro
`#define ETH0 13` está en `include/conf.h:47`. La razón de la extrañeza está comentada en el
propio `conf.h:140`: el tipo de device se llama `espeth`, no `ether`, así que el generador
no lo agrupa con los demás.

**Dónde se edita de verdad** (esto es lo que hay que retener): `system/conf.c` e
`include/conf.h` son **generados** por `compile/config/config` desde
`compile/platforms/esp32p4/xinu.conf`. Cualquier edición a mano la borra un `make`.
En `xinu.conf`:
- `espeth:` bloque de tipo, líneas 102-105 (`-i espethInit -o espethOpen -c espethClose
  -r espethRead -w espethWrite -n espethControl`),
- instancia `ETH0 is espeth on HARDWARE` línea 189,
- `#define NETHER 1` línea 274.

`getdev("ETH0")` devuelve 13. Regla del proyecto: en el código se usa `getdev()`, no la
macro, para no depender del orden del `devtab`.

Anillo de recepción: `ESPETH_RX_SLOTS = 8` (`include/espeth.h:46`; antes 4). Coste: 8 ×
`ESPETH_FRAME_MAX` ≈ 12,8 KB de `.bss`. Por qué 8: medido, con el lector de señal en marcha
`caidas_rx` subía de 102 a 129 en 8 s con 4 ranuras; 8 cubre la cola de 8 del propio
transporte esp_hosted.

---

## 3. Levantar la red, paso a paso

### 3.1 Automático (lo que hace el arranque)

`system/main.c` lanza la hebra de arranque, y desde ahí se puede invocar la secuencia. En
la consola:

```
xsh$ netboot
```

`shell/xsh_netboot.c:55-` ejecuta, en este orden y con los comandos de shell reales (así la
salida es idéntica a teclearlos, y un paso que falla corta la secuencia en vez de seguir
con una interfaz muerta):

1. `wifi up` — engancha el transporte SDIO al C6.
2. `wifi join <ssid> <pass>` — **reintenta 3 veces dentro del propio join**.
3. `netup ETH0` — sube el `netif`: `ethInit`/`espethInit`, ARP, rutas, DHCP y las hebras
   `netRecv`.
4. `telnetserver -n 1` — un servidor telnet: 1 hebra de sesión y, como no hay cola de
   accept, 1 cliente concurrente (ver §6.6).
5. `httpd` — el servidor HTTP embebido.

Credenciales: por defecto son las constantes de la misma función — `ssid`/`pass` en
[`shell/xsh_netboot.c:55`](../../shell/xsh_netboot.c) (AP actual: `PhMNet_2`, cambiada el
2026-11-04; antes `wifi2.4Ghz`), y `netboot [ssid] [pass]` las pisa sin recompilar. La
contraseña no se copia en este documento: el único sitio es el fuente. Está autorizado así
por el dueño del árbol (decisión de 2026-11-02). Si `netboot` reintenta, son 5 intentos
× 10 s; un `StaDisconnected reason=39` en el primer intento seguido de éxito en el segundo es
comportamiento preexistente del C6, no un fallo nuevo.

### 3.2 Manual, con otra AP

```
xsh$ netdown ETH0
xsh$ wifi join "otra-red" "su-password"
xsh$ netup  ETH0
xsh$ ifconfig                      # IP, máscara, gateway, MAC del C6
xsh$ ping 192.168.1.1              # 1 s por defecto
xsh$ netstat
```

Resumen medido en placa tras el arreglo de la cola RX: **200/200 pings, 0 % de pérdida**,
sin DUP, `rx_drops` acumulado = 4 y HTTP con mediana 16 ms.

### 3.3 Qué hace `netup` por dentro

[network/net/netUp.c](../../network/net/netUp.c): añade la red a la tabla de rutas
(`rtAdd`), instala el gateway (`rtDefault`) y crea las hebras de recepción —
`NET_NTHR = 1` hebra `netRecv` con `NET_RECV_STK = 8192` (macros de

`include/network.h:106-113`), nombre `espethrecv00`. **Sin estas hebras no hay red**:
las tramas entran al anillo de `espeth` y nadie las desmultiplexa.

---

## 4. Los programas de red y sus ejemplos (punto 3)

### 4.1 Inventario real de la shell

| comando | fichero | qué hace |
|---|---|---|
| `netboot [ssid] [pass]` | `xsh_netboot.c` | secuencia completa arriba |
| `netup [-v] dev` / `netdown dev` | `xsh_netup.c` / `xsh_netdown.c` | sube/baja un `netif`; `netup` también imprime `espeth_stats` |
| `ifconfig [-a dev ip mask gw]` | `xsh_ifconfig.c` | estado y reconfiguración del `netif` |
| `ping [-i n] [-c n] host` | `xsh_ping.c` | ICMP por la pila; usa el **RAW** device |
| `arp [-d] [-s ip mac]` | `xsh_arp.c` | tabla ARP |
| `route [-d ip] [-r ip másc gw]` | `xsh_route.c` | tabla de rutas (`route.c`) |
| `netstat` | `xsh_netstat.c` | `netif`, pool de buffers, listas de control, ARP, rutas |
| `tcpstat` | `xsh_tcpstat.c` | cada PCB TCP con su estado, ventanas y contadores |
| `udpstat` | `xsh_udpstat.c` | cada PCB UDP con puertos y colas |
| `dns [-s servidor] nombre` | `xsh_dns.c` | resolución; **hardcodea 8.8.8.8**, sin `resolvtab` (`xsh_dns.c:43-46`) |
| `telnet host [puerto]` | `xsh_telnet.c` | **cliente** |
| `telnetserver [-d dev] [-p puerto] [-n num]` | `xsh_telnetserver.c` | arranca servidores telnet |
| `telnetd` | `xsh_telnetd.c` | variante "daemon" (todas las ranuras TELNET0..3) |
| `httpd [-d dev] [-p puerto]` | `xsh_httpd.c` | servidor HTTP completo: estaticos desde la FAT + API REST de ficheros (`httpapi.c`) |
| `udpserver [puerto]` | `xsh_udpserver.c` | servidor de comandos por UDP en el 5000 + sesión de subida de firmware (§7) |
| `psram [free]` | `xsh_psram.c` | rango, libre/usado y bloques libres del heap de PSRAM |
| `wifi …` | `xsh_wifi.c` | 24 subcomandos del transporte/RPC (ver §6) |
| `wifi_scan` | `xsh_wifi_scan.c` | barrido clásico + `rpc_call()` sobre el RPC crudo |

**Existentes pero NO registrados** en la `commandtab` de `shell/shell.c:36-79` (victoria
rápida si quieres usarlos: tres entradas + `include/shell.h`):

- `shell/xsh_curl.c` — cliente HTTP completo por URL,
- `shell/xsh_wget.c` — descarga a fichero,
- `shell/xsh_ip.c` — `ip addr/route/neighbor`, más moderno que `ifconfig/route/arp`.

**No portado todavía** desde `xinu-s3-amp`: `xsh_nc.c`.  `xsh_udpserver.c` **sí** está
portado y validado en placa desde el 2026-11-04 (ver §7), y `httpapi.c` también (§5).  Lo
que sigue sin traer del S3 es la parte de terminal web del httpd (`/api/exec`, `/api/job`),
porque `memtty`/`webterm` no existen en el P4.

### 4.2 Ejemplos que se pueden escribir hoy

```
xsh$ netstat                         # ¿qué hay vivo?
xsh$ tcpstat                         # PCB por PCB
xsh$ ping -c 5 -i 2 8.8.8.8          # conectividad cruda por ICMP
xsh$ dns qoder.com                   # resuelve (contra 8.8.8.8)
xsh$ route                           # tabla; puerta de enlace por defecto
xsh$ arp                             # vecinos
xsh$ telnet 192.168.1.146            # sesión desde otra máquina
xsh$ httpd                            # escucha en 80; desde el PC:
```
```
$ curl -s http://192.168.1.146/ | head           # 200 OK, el /index.html de la SD
$ curl -s "http://192.168.1.146/api/files?path=/" # lista la tarjeta en JSON
$ curl -s -T pagina.html "http://192.168.1.146/api/file?path=/index.html"   # sube
$ time curl -s -o /dev/null http://192.168.1.146/   # 76.834 B en 0,52 s (medido)
$ python3 /tmp/p4udp.py 192.168.1.146 cmd PING INFO # consultas al udpserver (§7)
$ python3 /tmp/p4udp.py 192.168.1.146 up xinu.boot  # sube una imagen a /xinu.img
$ nc -lvu 1234                            # recibir un UDP que mande la placa
```

> La IP del ejemplo es la de **casa** (`192.168.1.146`).  Es de DHCP y cambia con
> la AP: en la del trabajo (`PhMNet_2`) la placa quedó en `172.16.0.117`.  La IP
> real la imprime `ifconfig` (línea `inet`) o el resumen de `netboot`
> (`netboot: ETH0 es …`).  Para
> un chequeo de la pila sin mirar tablas a mano existe `syscheck net` — netif,
> buffers libres del pool, ARP, rutas y contadores del espeth en un solo comando
> ([06-syscheck.md](06-syscheck.md) §4); medido el 2026-11-04 con la red del
> trabajo: 4/4 OK, 23 buffers libres de 24 en reposo y `drops 0`.

Raw / UDP / TCP **desde C** (no hay comando de shell genérico; `ping` usa RAW y `httpd`/
`telnetserver` usan TCP). El patrón mínimo, con las primitivas que ya están:

```c
/* --- cliente TCP activo --------------------------------------------- */
int dev = tcpAlloc();                       /* "asignar un puerto local" */
if (SYSERR == dev) return SYSERR;
if (OK != open(dev, NULL, &remoteip, REMOTE_PORT, NULL, TCP_ACTIVE)) { tcpFree(dev); }
write(dev, buf, len);
n = read(dev, buf, sizeof buf);
close(dev); tcpFree(dev);

/* --- servidor TCP pasivo -------------------------------------------- */
int ldev = tcpAllocPassive();               /* escucha, sin puerto dado  */
open(ldev, &iface->ip, NULL, port, NULL, TCP_PASSIVE);
/* cada hebra de servicio hace su propia copia: */
int sdev = tcpOpen(ldev, NULL, NULL, NULL, NULL);   /* bloquea hasta que entra un SYN */
```

Timeouts y opciones de un socket: `control(dev, TCP_CTRL_RCVTIMEO, ms, 0)` (también
`TCP_CTRL_KEEPALIVE`, `TCP_CTRL_OOBTEXT`, `TCP_CTRL_DELAY`, según
`include/tcp_ctrl.h`). **Sin `TCP_CTRL_RCVTIMEO` un `read()` en un cliente que se calla
bloquea la hebra para siempre** — es la causa del patrón "servidor que deja de aceptar".

UDP: `udpAlloc()` → `open(dev, &localip, NULL, LOCAL_PORT, NULL, UDP_PASSIVE)` para
escuchar, o `sendto()`/`recvfrom()` sobre el device ya abierto. RAW (ICMP): `rawAlloc()` +
`open()` con protocolo; `xsh_ping.c` es el ejemplo vivo y corto.

---

## 5. `httpd`: servir de la SD y la API de ficheros (punto 3)

Puertado del `httpd` de `xinu-s3-amp` **el 2026-11-04 y validado en placa**. Ya no es
el de la página embebida: `shell/xsh_httpd.c` sirve ficheros desde la FAT de la microSD y
`shell/httpapi.c` es la API REST de ficheros (`include/xsh_http.h` comparte el request
parseado). La página embebida `httpd_index[]` sigue ahí, pero degradada a **respaldo de
`/` y `/index.html`** para cuando la tarjeta todavía no tiene ese fichero.

Lo que cambió respecto al S3 al portarlo (no es reflujo de estilo, son dos capas del P4
que no existen allí):

| pieza del S3 | equivalente en el P4 | por qué |
|---|---|---|
| `fat_mounted()` / `fat_mount()` | `fs_mounted()` (`shell/fsutil.c:80`) | el FAT del P4 lo monta el device `FAT0` en el arranque; no hay montaña a demanda que hacer |
| `full_path(path)` | `fs_resolve(arg, out, sz)` (`fsutil.c:103`) | normaliza `.`/`..` y frena en la raíz, la misma propiedad de seguridad |
| `fs_remove_file()` | `fl_remove()` de la librería | el P4 no tiene ese helper |
| `fs_dir_is_empty()` | `dir_is_empty()` local en `httpapi.c` | ídem |
| `FS_BUF_LEN` | `FS_PATH_MAX` (128) | el techo de ruta del shell del P4 |
| `/api/exec`, `/api/job` | **caídos** | dependen de `webterm`/`memtty`, que en el P4 no existe (tarea #57) |

Medido en placa con el build de las 16:10 (IP `192.168.1.146` por DHCP en la AP `wifi2.4Ghz`):

```
PUT  /api/file?path=/index.html   -> 201 {"ok":true,"size":76834}  en 1,98 s
GET  /                            -> 200 text/html, 76.834 B, IDENTICO byte a byte (cmp)
GET  /                            -> 0,52 / 0,66 / 0,68 s en tres pasadas seguidas
GET  /api/files?path=/            -> JSON con los 8 entradas de la raiz
GET  /api/files?path=/../../      -> se queda en "/"  (no sale de la tarjeta)
POST /api/rmdir?path=/httpdtest&recursive=1 -> {"ok":true}
GET  /no-existe.html              -> 404 ;  POST /api/files -> 405 {"use GET"}
```

Nota de frescura sobre esas cifras: son de la primera versión del tablero (76.834 B).  El
`/index.html` que está servido ahora en la tarjeta es la misma página, ya con las mediciones de
`httpd` y de `udpserver` dentro.  La última pasada es a las 17:28 del 2026-11-04: **81.956 B**
(md5 `e94e8e2bab75ec5217a50dceb6ec58d7`); cambió solo la línea que identificaba la imagen
medida, porque el contador `#NNNN` de Xinu no vale como identidad (ver la nota de
[00-indice.md](00-indice.md)).  La `GET /` devolvió `cmp` idéntico al fichero del PC y
`/api/files` reporta para `index.html` 81.956 B.  Sobrescribir el tablero es además el
tercer ejercicio del defecto de truncado (§7.4): 81.629 → 81.749 → 81.956 B, siempre con la
talla exacta en la tarjeta.

Y tres cosas que interesan por lo que dicen del resto del sistema:

- **El servidor vuelve a escuchar solo**: tras el 404 del favicon la GET siguiente da
  200.  Lo consigue la hebra `httpdReleaser` (el `close()` de un TCP bloquea hasta el FIN
  del peer) y el `TCP_CTRL_RCVTIMEO` de 2 s sobre un `read()` que si no se queda mudo para
  siempre.
- **Una conexión cada vez**: dos GETs **paralelas** dan 200 y `000` (rechazada) — es la
  misma limitación del telnet con `-n 1` (§6.6), no un fallo del httpd.  Un navegador lo
  nota como peticiones serializadas; para varios clientes hay que subir `NTCP` y dar un
  socket a la escucha por cliente.
- **La pila no se aprieta**: `ps` marca `httpd` con **1.476 B de pico sobre 8.192 B**, y es
  porque el búfer del request (2 KB), el `http_request` y los búferes de la API son
  `static`: un solo servidor atiende de una en una, así que no necesitan pila por hebra.

### 5.1 Segundo servidor en otro puerto

El comando ya acepta `httpd [-d dispositivo] [-p puerto]` y resuelve la interfaz con
`getdev()`, pero **sigue habiendo uno solo**: `httpdRunning` (`xsh_httpd.c:80`) corta el
segundo arranque y los búferes `static` del request y de la API los comparten ambas hebras.
Para un segundo puerto hacen falta su propia bandera, su propio nombre de hebra y poner los
búferes por instancia (o un segundo juego).  El resto del patrón es el que ya está en el
árbol: `tcpAllocPassive()` → `open(..., TCP_PASSIVE)` → `control(TCP_CTRL_RCVTIMEO)` →
`httpdRelease()`.

### 5.2 Lo que queda por traer del S3

| pieza | estado |
|---|---|
| estáticos desde FAT + `Content-Type` | **hecho** |
| `PUT`/`DELETE`/`mkdir`/`rmdir` con `Content-Length` y `100-continue` | **hecho** |
| query string, 404, 405, 400 | **hecho** |
| terminal web (`/api/exec`, `/api/job`, `memtty`) | pendiente (tarea #57) |
| varios servidores a la vez / backlog TCP | pendiente; toca `NTCP` y el ciclo de vida del PCB |

---

## 6. esp_hosted / WiFi (punto 5) {#wifi}

### 6.1 Arquitectura

El C6 habla el protocolo **esp_hosted** por SDIO (función 1 del slot 1). Dos canales:

- **Datos**: tramas Ethernet en la cola TX/RX. `esp_hosted_packet_t` con cabecera + payload
  de hasta 1600 B, movidas por el bloque SDHOST con ventana DMA.
- **Control**: RPCs con TLV+protobuf. Petición id `N` → respuesta id `N + 256`.

Los RPCs que usa este port (IDs barridos y confirmados en placa):
`257` (STA rssi/link), `260` (config/estado), `278`, `280`, `282`, `283`, `284`, `350`
(scan), `387`; eventos `769`, `775`, `776`. Respuesta de RSSI = petición + 256 ⇒ `550`.

### 6.2 Comandos de WiFi

```
xsh$ wifi up          # engancha el transporte SDIO (slot 1) y reporta los registros del esclavo
xsh$ wifi ver         # primer RPC: versión del firmware del C6 (timeout 3 s)
xsh$ wifi diag        # relojes SDHOST compartidos, páginas DMA, registros fn1
xsh$ wifi scan        # lista las redes visibles (medido: 9 redes)
xsh$ wifi scan <n>    # detalle de una entrada del último barrido
xsh$ wifi signal      # RSSI del AP asociado -> porcentaje de calidad
xsh$ wifi join <ssid> <pass>
xsh$ wifi leave / down
xsh$ wifi stats       # contadores del transporte + espeth_stats
xsh$ wifi dump|probe|jam|poke|init|mac|sweep <de> <a>|sdio|fn1|dma|pio|slow|bus|dat|wires|hunt|hold|pads
```

Los últimos son la batería de diagnóstico que se construyó durante el bring-up (barrido de
IDs, prueba de que el esclavo parsea nuestros bytes, embudo de la cola SERIAL del esclavo,
comprobación de pads/velocidades del SDIO). Todos documentados en el `wifi_usage()` de
`shell/xsh_wifi.c:1369+`.

**Calidad de señal**: `wifiSignalQuality()` es una recta `-100 dBm → 0 %`, `-40 dBm → 100 %`,
con timeout de 1000 ms en la lectura (`shell/xsh_wifi_scan.c:663+`).

### 6.3 Cambiar de red

`wifi join "otra" "pass"` (intenta 3 veces) y luego `netup ETH0` para que DHCP pida otra
lease. Con `netboot otra-red pass` haces las dos cosas y relanzas los servidores. No hay
perfil ni lista de redes guardadas: la credencial activa es la última que se pasó.

### 6.4 La carrera que se arregló (y por qué era difícil de ver)

Síntomas medidos antes del arreglo: **37 % de pérdida de ping**, pings `DUP`, y
`rx_drops`/`descartadas` subiendo (49→60 y 102→129 en 8 s) con el HTTP en marcha.

Causa raíz, dos cosas a la vez:
1. La cola RX de esp_hosted (`head/tail/count`) no tenía sección crítica y la tocaban varios
   hilos: `netRecv`, la hebra del panel, la shell y los lazos de espera de RPC.
2. `esp_hosted_sdio_transport_poll()` compartía `frame_buf` entre hilos: la secuencia lee
   `INT_RAW`, lo limpia, pide longitud, baja la ventana y encola.

Arreglo (ya en el árbol, compilado, flasheado y revalidado):
- `esp_hosted_queue_push()`/`pop()` envueltos en `disable()/restore()`
  ([device/sdio-esp32p4/espHosted.c:82](../../device/sdio-esp32p4/espHosted.c#L82) y
  `:112`),
- `esp_hosted_poll()` con `sdmmcBusLock()/Unlock()` alrededor del `transport.poll`
  (`espHosted.c:284`),
- anillo de `espeth` 4 → 8 (`ESPETH_RX_SLOTS`),
- y una **entrada de RPC crudo** en la capa de control:
  `esp_hosted_ctrl_raw_request()` (`device/sdio-esp32p4/espHostedCtrl.c:2045`), wait-kind
  `ESP_HOSTED_CTRL_WAIT_RAW`, campo 3 = uid del controlador, respuesta `req_id + 256`, y
  `ESP_HOSTED_CTRL_ERR_BUSY` si ya hay una espera abierta. `xsh_wifi_scan.c:rpc_call()` es
  ahora un delegador de esa función. Con esto la shell **ya no vacía la cola de RX** para
  leer la señal, que es lo que provocaba la colisión con `netRecv`.

Pilas ajustadas en la misma etapa (`-fstack-usage`): `esp_hosted_dispatch_rx` gasta **1664 B**
de frame (lleva dentro un `esp_hosted_packet_t` de 1600 B de payload), así que `netRecv`
pide `NET_RECV_STK 8192` ([network/net/netUp.c:151](../../network/net/netUp.c#L151));
`tcpTimer` pasa de `INITSTK` a `TCP_TIMER_STK 8192`
([network/net/netInit.c:75](../../network/net/netInit.c#L75)); el panel `p4demoTask` a 8192
(`system/main.c:73`). Nota metodológica: **el "peak" que imprime `ps` no sirve para
dimensionar** — se muestrea en los cambios de contexto y no ve la cadena de llamadas profunda
(tcpTimer marcaba 100/8192 con una cadena real `tcpTick→tcpSend→espethWrite→SDIO`).

### 6.5 Lo que NO puede hacer este WiFi (importante para tus apps)

El transporte esp_hosted **solo mueve tramas Ethernet**. En el árbol del SDK
(`/home/daniel/esp32-riscv-bare-metal-sdk-main/drivers/esp_hosted`) no hay ni una sola API de
modo monitor, sniffer, inyección, deauth ni lectura de tramas 802.11 crudas — barrido de
`power_save|set_ps|dtim` = 0 resultados. Consecuencia directa:

- la "app para vulnerar redes para aprender" **no se puede hacer** con este hardware como
  radio de inyección/monitor: no hay forma de poner el C6 en modo monitor desde este port.
  Lo que sí se puede: barrido normal (`wifi scan`), asociarse, y escaneo de puertos/servicios
  por TCP/UDP desde la pila — que es una herramienta de red legítima y suficiente para
  aprender.
- la latencia en reposo (~81 ms de p50 con pings cada 200 ms; 7,5 ms con pings cada 50 ms;
  mínimo 2,15 ms) es **power save del C6**: la ruta local es rápida y lo que espera es la
  baliza/DTIM del AP. Bajarla pide una API de power save que el SDK bare-metal no expone
  (tarea #61). La única vía realista sería una petición RPC propietaria, no documentada.

### 6.6 Telnet: ciclo de vida del servidor y el estado real del #62

**Cómo es el modelo (y por qué el puerto 23 se niega a veces sin que haya bug).**
En esta pila no existe `accept()` ni cola de pendientes: `device/tcp/tcpRecvListen.c:75`
transforma el propio TCB que estaba en `LISTEN` en `TCP_SYNRECV`, es decir **un PCB de
escucha = un cliente**. `netboot` arranca `telnetserver -n 1` (`shell/xsh_netboot.c:64`),
así que mientras hay una sesión puesta no queda ningún TCB en `LISTEN` en el 23; el SYN del
segundo cliente no encuentra emparejamiento nivel 1 en `device/tcp/tcpDemux.c` y
`device/tcp/tcpRecv.c:57` responde con RST → el programa local lo reporta como
`Connection refused`. Eso es comportamiento por diseño, no el #62.

Para tener N sesiones simultáneas sin tocar código hay N hebras servidoras, cada una con su
TCB pasivo: `telnetserver -n N` (tope `NTELNET 4` en `include/conf.h:88` y `NTCP 10` en
`:87`; `xsh_telnetserver.c:193-204` recorta el número). El demux reparte cada SYN nuevo
entre los slots que queden en `LISTEN`, por índice ascendente.

Es el patrón del port S3, comprobado con `diff` entre `/home/daniel/s3-xinu` y este árbol:
`tcpRecvListen.c` idéntico, `telnetWrite.c` idéntico byte a byte y `telnetServer.c` igual
salvo la constante de pila de la hebra shell (`INITSTK` → `TELNET_SHELL_STK`). El archivo
`device/tcp/tcpAllocPassive.c` incluso conserva en su cabecera la firma "ported to
ESP32-S3".

**Medido el 2026-11-04 (PC y placa en la misma /24, `172.16.0.102` y `172.16.0.117`).**
Cuatro ciclos conectando, mandando `syscheck mem` y matando el socket con `SO_LINGER` a los
3 s — o sea con salida en vuelo, que es la condición que se creía letal —, dos con FIN
limpio y dos con RST: el 23 volvió a aceptar **en 1 s en los cuatro casos** y las
reconexiones salieron 8/8. Con `mode clean` y `mode rst` del sondeador
`/tmp/p4teln.py`, 3 intentos encadenados cada uno, también OK. **El #62 no se reproduce en
el build actual.**

Por qué se recupera: el RST con el socket todavía en `SYNRECV` y pasivo lo devuelve
directamente a `LISTEN` (`device/tcp/tcpRecvOther.c:55-58`); si la conexión ya estaba
establecida, el RST libera el TCB y la hebra shell sale por EOF — el camino `read_err:` de
`device/telnet/telnetRead.c:358` señaliza `isem` y fija `ieof`, `shell/shell.c:131` rompe el
bucle de lectura, la hebra termina y `system/kill.c:38` manda el `tid` al padre;
`device/telnet/telnetServer.c:114` deja entonces el bucle `recvclr()`, cierra y vuelve a
`tcpAllocPassive()`. Ese arreglo de la ruta de lectura es lo que cerró el agujero que se
documentó cuando el fallo era persistente.

**Riesgos que quedan, escritos pero sin tocar** (vienen heredados del S3 tal cual):

- `device/telnet/telnetWrite.c:50` y `:62` devuelven `SYSERR` sin `signal(tntptr->osem)`
  del `wait()` de la línea 39. Si una escritura falla, la **siguiente** escritura de esa
  sesión se bloquea para siempre; la hebra shell nunca muere, el padre nunca recibe el
  mensaje y el 23 queda muerto hasta reset. No se ha disparado en ninguna de las pruebas de
  arriba, que exigen que el fallo caiga en la ruta de error con el peer ya ido. Son dos
  líneas, pero viven en `device/`: requieren autorización explícita.
- `telnetOpen.c:71-72` recrea `isem` y `osem` en cada `open()`, así que una fuga de una
  sesión no envenena a la siguiente; a cambio `telnetClose.c` no hace `semfree()` de esos
  dos, con lo que cada ciclo de conexión pierde dos ranuras de semáforo.
- `telnetFlush.c:31` y `:36` devuelven `SYSERR` con las interrupciones aún desactivadas
  (`disable()` en la línea 27 sin su `restore(im)`). Es el camino del reloj: si alguien
  escribe o hace `TELNET_CTRL_FLUSH` sobre una ranura que ya no está en
  `TELNET_STATE_OPEN` —por ejemplo un `telnetserver -h` que cierra dispositivos mientras las
  hebras servidoras siguen vivas—, el kernel se congela en vez de perder solo la sesión.

---

## 7. `udpserver`: comandos y subida de firmware por UDP {#udpserver}

Portado del `udpserver` de `xinu-s3-amp` **el 2026-11-04 y validado en placa** (kernel de
315.820 B; la placa responde `VERSION` = `#1564` y su banner de arranque dice `#1555` — el
contador de Xinu es **por objeto, no por imagen**: ver la nota de [00-indice.md](00-indice.md)).  Fichero: `shell/xsh_udpserver.c`.  Escucha en UDP **puerto 5000**
(`UDPSERVER_PORT`, `xsh_udpserver.c:63`) y hace dos cosas: responde consultas de estado sin
necesidad de telnet ni de HTTP, y recibe una imagen de firmware bloque a bloque hasta dejarla
en `/xinu.img` de la microSD.

`netboot` lo lanza junto con el telnet y el httpd (`xsh_netboot.c:147`), pero **de forma no
fatal**: se crean tres hebras servidoras y solo hay dos dispositivos UDP (`NUDP 2`), y DHCP o
un `dns` siguen necesitando uno.  Si no hay ranura, el arranque continúa y lo avisa; el
servidor se puede lanzar a mano con `udpserver [puerto]`.

### 7.1 El modelo del bucle (por qué pasivo y por qué BIND por petición)

No hay `accept()` en UDP, como no lo hay en TCP en esta pila.  El patrón, en orden
(`udpServer()` en `xsh_udpserver.c:876`):

```c
udpdev = udpAlloc();                                   /* ranura del pool NUDP   */
open(udpdev, &iface->ip, NULL, port, 0);               /* IP+puerto locales, sin remoto */
control(udpdev, UDP_CTRL_SETFLAG, UDP_FLAG_PASSIVE, 0);
/* ... */
n = read(udpdev, udpRx, sizeof udpRx);                 /* pseudo + cabecera + payload */
control(udpdev, UDP_CTRL_BIND, pkt->srcPort, (long)&src);   /* destino de la respuesta */
/* write de la reply */
control(udpdev, UDP_CTRL_BIND, 0, (long)NULL);         /* se suelta el endpoint    */
```

El flag que importa es `UDP_FLAG_PASSIVE` (`include/udp.h`): en modo pasivo `udpRead()`
**no consume la cabecera**, entrega el `udpPseudoHdr` (12 B: origen y destino de 4 B cada uno,
`zero`, `proto` y la longitud UDP) seguido del `udpPkt` (`srcPort/dstPort/len/chksum`, 8 B) y el
payload.  Ahí está la dirección del cliente,
y de ahí salen a la vez el `netaddr` para el `BIND` de la respuesta y la comprobación de
"esta subida la abrió este cliente".  En modo no pasivo `udpWrite` exige el endpoint remoto
ya enlazado — por eso el `BIND` va **dentro** de cada petición y se suelta al terminar: si no
se soltara, la siguiente consulta de otra IP escribiría contra el cliente anterior.

La trama entrante se corta en `UDP_MAX_DATALEN` (1024 B) y el búfer de lectura es
`sizeof(udpPseudoHdr) + UDP_HDR_LEN + UDP_MAX_DATALEN`; la respuesta cabe en
`UDPSERVER_REPLY` (1024 B).

### 7.2 Comandos de estado

| petición (texto) | respuesta | de dónde sale |
|---|---|---|
| `PING` | `PONG <clktotal>` | reloj del kernel en ms |
| `INFO` | tarjeta completa | `VERSION` + `clktotal` + `udpAppendNet` + `memlist.length` + familia PSRAM |
| `VERSION` / `UPTIME` | una línea | `VERSION` / `clktotal` |
| `MEM` | `heap_range/size/free/used` + PSRAM | L2MEM es `[memheap, platform.maxaddr)`, el libre total está en la cabecera de `memlist` |
| `PSRAM` | `psram_range/size/free/used` | `[psminheap, psmaxheap)` y `psramlist.length` — el equivalente exacto del comando `psram` |
| `PS` | tabla `TID NAME STATE PRIO PPID CPU% STACK` | `thrtab[]` con `cpu_ticks`, `stk_peak`, `stklen`; ventana de **1 s** con `sleep(1000)` entre dos lecturas |
| `NET` | `ip/netmask/gateway/mac/mtu` | `netLookup(getdev("ETH0"))` |
| `SD INFO` | `blocks/size_mb/type/bus/rca/fat` | el driver: `sd_present/sd_blocks/sd_hc/sd_width/sd_rca` (`include/sdblock.h`) + `fs_mounted()` |
| `REBOOT` | `OK rebooting` y reset | `xsh_reset`: `LP_AON_CLKRST` + `HPCPU_RESET_CTRL0` (offset `0x14`), bit 13 `HPCORE0_SW_RESET`, escribir-1-para-disparar (`shell/xsh_reset.c:24-25`) |
| `HELP` | lista de comandos + nota de `apply=blocked` | — |

Medido tal cual salió (`SD INFO`): `blocks: 122138624`, `size_mb: 59638`, `SDHC/SDXC`,
`4-bit`, `rca: 0x59b4`, `fat: mounted`.  `PSRAM`: rango `0x48027000-0x49ff8000`, 33.361.920 B
tras devolver los 992 KB de la reserva del ULP.

**Del S3 no se portaron GPIO, TFT, RGB ni BUTTON.**  En el S3 el GPIO usa `xt_gpio_*`
(Xtensa) y el P4 tiene otro controlador, además de no haber ningún pin que merezca la pena
mover por la red; y TFT/RGB/BUTTON no existen como devices en este árbol — el LCD lo posee el
`dash`, y pintar por UDP encima del dash es una carrera segura.  Está escrito en la cabecera
del fichero para que nadie lo vuelva a intentar por copiar del S3.

### 7.3 La sesión de firmware

Un datagrama por bloque, **stop-and-wait** (el cliente no manda el siguiente hasta recibir el
ack), y los bloques entran **crudos en el mismo datagrama** que su cabecera ASCII:

| petición | respuesta correcta |
|---|---|
| `FW_BEGIN <talla>` | `OK FW_BEGIN size=… block=960 temp=/xinu.tmp` |
| `FW_DATA <seq> <len>\n<bytes>` | `OK FW_DATA <seq> <recibido>/<talla>[ complete]` |
| (seq repetido) | `OK FW_DATA <seq> duplicate <recibido>/<talla>` — **sin reescribir** |
| `FW_STATUS` | `FW_STATUS IDLE [staged=/xinu.img size=…]` o `FW_STATUS ACTIVE size=… received=… next=… block=960 temp=/xinu.tmp` |
| `FW_END` | `OK FW_END size=… staged=/xinu.img apply=blocked` |
| `FW_ABORT` | `OK FW_ABORT` (y borra `/xinu.tmp`) |

Bloque de **960 B** (`FW_BLOCK_SIZE`): deja sitio para la cabecera ASCII más larga dentro de
un payload UDP, cuyo techo es `UDP_MAX_DATALEN` (1024 B), así que **un bloque = un datagrama**,
sin fragmentación IP y sin partir la sesión en dos lecturas.  El búfer de lectura añade además
los 12 B del pseudo-header y los 8 B de la cabecera UDP (`UDPSERVER_RX`).  Techo de imagen
`FW_MAX_IMAGE = 0x200000` (2 MB).

Tres decisiones del protocolo que están ahí por algo:

- **`seq` consecutiva desde 0, y el duplicado se confirma sin escribir.**  Es lo que hace
  segura la retransmisión: si el ack se pierde (no el bloque), el cliente reenvía el mismo
  `seq`, recibe `duplicate`, y `/xinu.tmp` no queda con el bloque doble.  Sin esta rama, un
  ack perdido corrompe la imagen.
- **La sesión es de un cliente** (`fw.client` + `fw.client_port`, comparados con
  `netaddrequal`).  Otro PC que intente `FW_DATA` en medio recibe `ERR FW_CLIENT`; un
  `FW_BEGIN` nuevo sí puede robar la sesión, y para eso tira el temporal viejo antes de abrir
  el fichero.
- **`FW_END` no reinicia.**  El S3 remataba con un reset; aquí reiniciar no cambiaría nada
  (no hay aplicador) y costaría la sesión y la IP.  Aplica el reset quien quiera, con
  `REBOOT` o por consola.

### 7.4 El final: no hay `fl_rename`, y la FAT no trunca

Dos hallazgos del port, ambos con su arreglo ya en el árbol:

| lo que hacía el S3 | el P4 | qué se hace aquí |
|---|---|---|
| `fl_rename("/xinu.tmp", "/xinu.img")` | **la librería FAT del P4 no tiene `fl_rename()`** | `fl_remove(dest)` → `fs_copy(tmp,dest)` → verificar talla **por nombre** → `fl_remove(tmp)`, todo dentro de un solo `fatLibLock()/fatLibUnlock()` |
| `fl_fopen(path,"w")` trunca | `fat_filelib.c:735` marca `FILE_ERASE` y **nadie lo lee**: sobre un nombre existente cae en la rama "Write Existing" (`:791-795`) y devuelve el fichero con su `filelength` y su cadena de clusters viejos | borrar el destino **antes** de abrir/copiar |

El monitor de la librería sí permite el candado envolvente: en este puerto los monitores de
Xinu **son reentrantes para el dueño** (`system/lock.c`: `if (thrcurrent == monptr->owner)
(monptr->count)++`), así que la secuencia va bloqueada de principio a fin aunque cada `fl_*`
vuelva a entrar.  No es atómico como un renombre de entrada de directorio: si el corte cae en
mitad de la copia queda un `/xinu.img` a medias — por eso la verificación es **sobre el nombre
final** y, si falla, se borra (`ERR FW_FINALIZE`).  Nunca queda una imagen parcial con el
nombre que leería un futuro cargador.

El segundo hallazgo es un defecto con efectos fuera del `udpserver`: **cualquier sobrescritura
más pequeña que el fichero anterior deja la cola vieja y la talla vieja**.  El mismo `fl_remove`
previo está ahora en el `PUT` de `httpapi.c` (con su comentario apuntando a `fat_filelib.c`).
Medido después del arreglo: `PUT` de 4096 B y encima `PUT` de 100 B → `ls -l` marca **100 B**
y el contenido descargado es idéntico byte a byte.  La evidencia de antes del arreglo es la de
la subida de firmware: un `FW_END` de 4096 B sobre el `/xinu.img` de 315.820 B daba
`ERR FW_FINALIZE` porque el directorio seguía contando la talla vieja.

Y la misma regla afecta al estado del `apply`: sigue **bloqueado por arquitectura**, no por
olvido.  No hay driver de escritura/borrado de flash en este árbol y el SBL no mira la SD
([05 §6](05-roadmap-y-conf.md)).  `FW_END` **apila** la imagen y lo dice en la respuesta.

### 7.5 Medido en placa

Subida de la imagen del propio kernel (`xinu.boot`, 315.820 B = 329 bloques) con el cliente de
prueba `stop-and-wait` (`/tmp/p4udp.py up`):

```
BEGIN  -> OK FW_BEGIN size=315820 block=960 temp=/xinu.tmp
DATA 0 … DATA 328 -> OK FW_DATA … complete          4,45 s en total
ack mas lento: 0,04 s
END    -> OK FW_END size=315820 staged=/xinu.img apply=blocked
```

Comprobación de integridad **por la otra vía**, sin fiarse del servidor: se descarga
`GET /api/file?path=/xinu.img` del `httpd` y se compara con el fichero local → `cmp` idéntico,
`md5 6383d3d4e02162c9ddd637ed17770c51` en los dos lados.  O sea: el camino
UDP→FAT→HTTP conserva la imagen bit a bit, que es lo que necesita un futuro aplicador.

Matriz de errores (todos probados contra la placa el 2026-11-04):

| caso | respuesta |
|---|---|
| `FW_DATA`/`FW_END` sin sesión | `ERR FW_IDLE` |
| `FW_BEGIN 0` / `FW_BEGIN > 2 MB` | `ERR FW_SIZE max=2097152` |
| `seq` saltada | `ERR FW_SEQ expected=… got=…` |
| `<len>` del header ≠ bytes del datagrama | `ERR FW_LEN header=… actual=…` |
| `len` 0 o > 960 | `ERR FW_BLOCK max=960` |
| bloque que excede la talla declarada | `ERR FW_OVERFLOW received=… len=… size=…` |
| cliente distinto en mitad de la sesión | `ERR FW_CLIENT` |
| ack perdido → reenvío del mismo `seq` | `OK FW_DATA … duplicate …`, sin datos duplicados |
| sesión cortada y `FW_BEGIN` de nuevo | recupera: borra el temporal y vuelve a `size=` |
| `FW_END` con destino ya existente más grande | `OK FW_END` (tras el `fl_remove`); antes daba `ERR FW_FINALIZE` |

Pila de la hebra (los tres búferes grandes -- `udpRx`, `udpLine`, `udpRep` -- son `static` a
nivel de fichero, el mismo criterio que en el `httpd`, y por eso el pico no depende de la talla
del payload):

```
udpserver 276/12288 B   justo despues del arranque
udpserver 1188/12288 B  mientras sirve un PS (la tabla entera)
httpd     1476/8192  B  (el mismo patron de buffers static)
```

### 7.6 Limitaciones aceptadas (y una que no es del servidor)

- **Un cliente cada vez.**  Un solo dispositivo UDP y bucle secuencial: mientras una subida
  está en curso, `PING` de otro PC no se atiende hasta el fin del bloque.  Es el precio de no
  tocar `NUDP`.
- **`PS` se trunca por el techo de 1024 B de la respuesta** (`UDPSERVER_REPLY`).  Una fila son
  ~40 B, así que la tabla entera cabe mientras haya ~24 hebras vivas; con más se corta a mitad
  (el `udpAppend` simplemente deja de escribir cuando el búfer se llena).  Para la vista
  completa está el `ps` de la shell o por telnet.
- **El primer datagrama tras un rato en reposo contesta en más de 4 s.**  No es el servidor:
  es el C6 dormido (#61).  Un cliente que no lo sepa lo toma por pérdida y desfasa todas las
  lecturas siguientes (los `cmd` del script imprimían cada respuesta bajo la pregunta
  siguiente); `/tmp/p4udp.py` lo resuelve con un `PING` de calentamiento antes de la sesión y
  vaciando el socket.  Con la sesión ya caliente el ack más lento medido fue 0,04 s; en la
  primera pasada hubo una punta de >6 s en el seq 175 (asignación de clusters), que es
  exactamente el caso para el que existe la rama de duplicado.
- **El cliente de prueba vive en `/tmp`** (`p4udp.py`, modos `cmd` / `raw` / `up`;
  `p4fwtest.py` para la matriz).  No hay cliente de shell en la placa: para bajar una imagen
  desde otra máquina hace falta un PC, o una app `elfrun` en el futuro.

