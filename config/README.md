# /config — configuración del kernel (JSON)

Estos ficheros se copian a la **raíz de la microSD** en `/config/`. El kernel
los lee **una vez al arranque** (`main()` → `kconfig_load()`), antes de que la
tarea NET levante el WiFi (en ese momento la SD todavía está en el bus SDMMC
compartido). Si un fichero no existe o su JSON es inválido, el documento queda
sin cargar y el código usa sus valores por defecto compilados.

| Fichero | Documento | Uso |
|---|---|---|
| `kernel.json` | `kernel` | `hostname`, `prompt` |
| `wifi.json`   | `wifi`   | `ssid`, `pass` (tiene prioridad sobre `/wifi.conf`) |
| `net.json`    | `net`    | p. ej. `httpd`, `telnet` |
| `user.json`   | `user`   | libre, para scripts del usuario |
| `init.cfg`    | (texto)  | arranque: `start`, `run`, `svc`, `delay` (ver `init.cfg`) |

## Uso desde el shell

```
config                     # lista los documentos y si cargaron
config show wifi           # imprime el JSON del documento
config get wifi.ssid       # valor de una clave (camino con puntos)
config set wifi.ssid MiRed # fija y guarda en la SD
config set net.httpd 1
config save kernel         # reescribe el documento
config load                # recarga todos los documentos de la SD
```

## Uso desde C

```c
#include <kconfig.h>

const char *s  = kconfig_str ("wifi.ssid", "def");
long        n  = kconfig_int ("net.port", 8080);
int         b  = kconfig_bool("kernel.telnet", 1);
```

El codec es `libjson` (`include/json.h`): parser/escritor zero-copy, sin heap y
sin primitivas del scheduler (se puede usar también fuera de tareas).

## Nota sobre el bus SDMMC

La microSD (slot 0) y el ESP32-C6 (slot 1) **comparten el controlador SDMMC**,
serializado por el monitor del kernel (`system/sdmmcbus.c`). Por eso la config
se lee al arranque: es el momento en que la tarjeta está segura. `wifi.json`
también se lee al principio del bring-up del WiFi (antes de cortar la
alimentación del slot).
