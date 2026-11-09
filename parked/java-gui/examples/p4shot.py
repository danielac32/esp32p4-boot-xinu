#!/usr/bin/env python3
"""p4shot - foto de la pantalla de la placa, leida del espejo de fotograma.

LVGL no tiene framebuffer completo: pinta en dos franjas de 320xDRAW_LINES
(device/lvgl/lvglport.c) y vuelca cada area por SPI2, o sea que en ningun sitio
hay una imagen de lo que se esta viendo.  `lvgl_shadow` si: lvgl_flush_cb copia
cada area invalidada a un lienzo de 320x480x2 B en PSRAM y aqui se lee por
telnet con `memdump -f`, que es la unica forma de validar la interfaz sin
depender de que alguien mire el panel.

RGB565 little endian (lv_conf.h: LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 0).

La direccion del SIMBOLO lvgl_shadow cambia con cada build del kernel:
  nm compile/xinu.elf | grep lvgl_shadow
y lo que se lee ahi es un PUNTERO a PSRAM, que pone el psram_malloc del arranque.

Uso:  python3 p4shot.py [salida.png] [--host 172.16.0.117] [--sym hex]
      [--y0 228 --alto 112]   (banda: el cuadro entero tarda ~1 min, una fila
                               de fichas unos 10 s)
"""
import argparse
import re
import socket
import struct
import sys
import time

from PIL import Image

W, H = 320, 480
N = W * H * 2
PROMPT = b"xsh$"
# El hexdump de Xinu suelta "08004000  aa bb .. hh  " (dos espacios tras la
# direccion y uno extra a mitad de linea), asi que se toman los pares sueltos.
HEAD = re.compile(r"^([0-9a-f]{8})\s")
BYTE = re.compile(r"^[0-9a-f]{2}$")


def hexline(line):
    """(direccion de la linea, [bytes]) o None si no es una linea de volcado.

    Se USA la direccion: la consola va mezclada con los kprintf del launcher
    ("workspace: evt cod...") y con reenvios del comando, y cualquier linea que
    se anexe a ciegas desplaza el resto del lienzo --con eso el cuadro salia
    cizallado en diagonal, no con un fallo de la placa.
    """
    m = HEAD.match(line.strip())
    if not m:
        return None
    data = [int(t, 16) for t in line.split()[1:] if BYTE.match(t)]
    if not data:
        return None
    return int(m.group(1), 16), data


def talk(s, cmd, tmo):
    """Manda `cmd` y devuelve lo que salio hasta el prompt de la shell."""
    s.sendall((cmd + "\r\n").encode())
    data = b""
    fin = time.time() + tmo
    while time.time() < fin:
        try:
            c = s.recv(65536)
        except socket.timeout:
            break
        if not c:
            break
        data += c
        if data.rstrip().endswith(PROMPT):
            break
    return data.decode("utf-8", "replace")


def readmem(s, addr, count, tmo, step=8192):
    """Cuadro de `count` bytes en `addr`, puesto por su direccion real.

    Devuelve (buf, huecos).  Un hueco es una linea que no llego: se queda en
    negro en vez de desplazar lo demas, que es lo que hace legible el PNG.
    """
    out = bytearray(count)
    vistos = 0
    for pos in range(0, count, step):
        n = min(step, count - pos)
        raw = talk(s, "memdump -f 0x%x %d" % (addr + pos, n), tmo)
        got = 0
        for line in raw.splitlines():
            e = hexline(line)
            if e is None:
                continue
            off = e[0] - addr
            if off < 0 or off + len(e[1]) > count:
                continue
            out[off:off + len(e[1])] = bytes(e[1])
            got += len(e[1])
        vistos += got
        if got < n:
            print("  faltan %d B en 0x%x" % (n - got, addr + pos),
                  file=sys.stderr)
    return out, count - vistos


def topng(raw, path, alto):
    img = Image.new("RGB", (W, alto))
    px = img.load()
    for y in range(alto):
        base = y * W * 2
        for x in range(W):
            v = struct.unpack_from("<H", raw, base + x * 2)[0]
            px[x, y] = (((v >> 11) & 31) * 255 // 31,
                        ((v >> 5) & 63) * 255 // 63,
                        (v & 31) * 255 // 31)
    img.save(path)
    print("  [ok]    %s (%d filas)" % (path, alto))


def puntero(s, sym):
    raw = talk(s, "memdump -f 0x%x 4" % sym, 8.0)
    for line in raw.splitlines():
        e = hexline(line)
        if e is not None and e[0] == sym and len(e[1]) >= 4:
            return struct.unpack("<I", bytes(e[1][:4]))[0]
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="/tmp/placa.png")
    ap.add_argument("--host", default="172.16.0.117")
    ap.add_argument("--sym", type=lambda x: int(x, 16), default=0x4ff432b8,
                    help="direccion del simbolo lvgl_shadow en .bss")
    ap.add_argument("--tmo", type=float, default=150.0)
    ap.add_argument("--y0", type=int, default=0, help="fila inicial de la banda")
    ap.add_argument("--alto", type=int, default=H, help="filas a leer")
    a = ap.parse_args()

    a.y0 = max(0, min(a.y0, H - 1))
    a.alto = max(1, min(a.alto, H - a.y0))
    n = a.alto * W * 2

    s = socket.create_connection((a.host, 23), timeout=a.tmo)
    s.settimeout(a.tmo)
    time.sleep(0.5)
    try:
        s.recv(65536)
    except OSError:
        pass

    ptr = puntero(s, a.sym)
    if ptr == 0:
        s.close()
        print("no se lee el puntero: simbolo viejo, refrescar --sym con nm",
              file=sys.stderr)
        return 1
    print("espejo en 0x%x, filas %d..%d" % (ptr, a.y0, a.y0 + a.alto - 1))
    t0 = time.time()
    fb, huecos = readmem(s, ptr + a.y0 * W * 2, n, a.tmo)
    s.close()
    print("  %d B en %.1f s, %d huecos" % (len(fb), time.time() - t0, huecos))
    topng(fb, a.out, a.alto)
    return 0


if __name__ == "__main__":
    sys.exit(main())
