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
| `httpd [-p puerto]` | `xsh_httpd.c` | servidor HTTP mínimo, un `index` embebido |
| `wifi …` | `xsh_wifi.c` | 24 subcomandos del transporte/RPC (ver §6) |
| `wifi_scan` | `xsh_wifi_scan.c` | barrido clásico + `rpc_call()` sobre el RPC crudo |

**Existentes pero NO registrados** en la `commandtab` de `shell/shell.c:36-79` (victoria
rápida si quieres usarlos: tres entradas + `include/shell.h`):

- `shell/xsh_curl.c` — cliente HTTP completo por URL,
- `shell/xsh_wget.c` — descarga a fichero,
- `shell/xsh_ip.c` — `ip addr/route/neighbor`, más moderno que `ifconfig/route/arp`.

**No portados todavía** desde `xinu-s3-amp` (quedan tareas abiertas): `xsh_nc.c`,
`xsh_udpserver.c` y `httpapi.c`/REST. Ver [05-roadmap-y-conf.md](05-roadmap-y-conf.md).

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
$ curl -s http://192.168.1.146/ | head        # 200 OK, ~874 B
$ time curl -s -o /dev/null http://192.168.1.146/   # mediana 16 ms
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

## 5. "¿Cómo hago otro servidor que sirva un HTML en otro puerto?" (punto 3)

Hoy `httpd` **no sirve ficheros**: responde siempre `httpd_index[]`, un `const char[]`
embebido en el binario (`shell/xsh_httpd.c:45`), con `Content-Type: text/html`. Hay dos
caminos según lo que quieras.

### 5.1 Camino rápido: cambiar el contenido (sin tocar nada más)

Sustituir el array `httpd_index[]` por el tuyo (mismo `Content-Length` calculado con
`sizeof(...) - 1`). Sirve para un dashboard fijo. Limitación: está en `.rodata`, o sea en
**flash**, y solo puede cambiar recompilando.

### 5.2 Segundo servidor en otro puerto, con ficheros de la SD

Este es el patrón que responde literalmente a tu pregunta. **Propuesta** — no está en el
árbol; el andamiaje que usa sí existe y está probado en `xsh_httpd.c:161-206`:

```c
/* 1. una hebra por servidor, con su propio puerto */
tid_typ tid = create(httpd2Server, 8192, INITPRIO, "httpd8080", 1, (int)8080);

/* 2. dentro de la hebra: */
int tcpdev = tcpAllocPassive();
struct netif *iface = netQuery(getdev("ETH0"));
open(tcpdev, &iface->ip, NULL, 8080, NULL, TCP_PASSIVE);   /* <-- el puerto, aquí */

for (;;) {
    int fd = tcpOpen(tcpdev, NULL, NULL, NULL, NULL);       /* acepta */
    control(fd, TCP_CTRL_RCVTIMEO, 5000, 0);
    char req[256];
    int n = readline(fd, req, sizeof req);                  /* "GET /x.html HTTP/1.1" */
    while (n > 0) { n = readline(fd, req, sizeof req);      /* traga cabeceras */
        if ('\r' == req[0] || '\n' == req[0]) break; }      /* hasta la línea vacía */
    /* servir desde FAT: */
    int sd = open("/x.html", O_RDONLY);                    /* requiere montar FAT */
    snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n", size);
    writen(fd, h, strlen(h));
    while ((n = read(sd, buf, sizeof buf)) > 0) write(fd, buf, n);
    close(sd); close(fd); tcpFree(fd);
}
```

Lo que hay que respetar para que no se rompa lo que ya funciona:

1. **No reutilices el estado global de `xsh_httpd.c`**: `httpdRunning` (línea 71) y el nombre
   de hebra `"httpd"` (línea 309) impiden dos instancias. Un segundo servidor necesita su
   propia bandera y su propio nombre, o generalizar el comando a `httpd [-p puerto]`.
2. **La hebra de servicio tiene que morir limpiando**: el patrón del árbol es una hebra
   `httpdReleaser` creada en `xsh_httpd.c:119` con `MINSTK` (que `MINSTK` mínimo
   `4*1024`, `include/thread.h:51`) porque 512 B en la pila de una hebra no dan. Esa hebra es
   la que cierra el socket cuando el peer se va — herédala.
3. **Timeout de lectura** (`TCP_CTRL_RCVTIMEO`) como red de seguridad: con `-n 1` el PCB que
   escucha es el PCB de la conexión (§6.6), así que un cliente que conecta y se queda mudo
   tiene el puerto 23 para él solo hasta que se vaya. `httpd` sí lo pide
   (`shell/xsh_httpd.c:175`, `control(tcpdev, TCP_CTRL_RCVTIMEO, HTTPD_RCVTIMEO, 0)`) por la
   misma razón; el servidor telnet no lo usa.
4. `readline()` en Xinu no quita el `\n`: hay que limpiarlo antes de parsear el path.
5. **FAT desde TCP es caro**: cada `open()` de la SD toma `sdmmcBusLock()`, que compite con
   el C6 (slot 1). Para servir un dashboard con tráfico continuo, cachea el fichero en
   PSRAM o sírvelo desde `.rodata`.

### 5.3 Lo que falta para un httpd "completo"

Estado actual y lo que hay que añadir (tarea #57):

| pieza | hoy | falta |
|---|---|---|
| método | solo `GET` | `POST` (leer `Content-Length` y el cuerpo) |
| contenido | `httpd_index[]` fijo | ficheros desde FAT + tabla de `Content-Type` |
| rutas | cualquier path devuelve el index | 404, directorio, query string |
| API | no | `GET /api/mem`, `/api/ps`, `/api/wifi` (REST) — `httpapi.c` existe en el S3 |
| terminal web | no | `memtty` + WebSocket-like por chunked; el S3 lo trae |
| nº de servidores | 1 (global `httpdRunning`) | array de estados por puerto |

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
(`system/main.c:91`). Nota metodológica: **el "peak" que imprime `ps` no sirve para
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
