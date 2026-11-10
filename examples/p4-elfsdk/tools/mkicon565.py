#!/usr/bin/env python3
"""Convierte una imagen a icon565.bin (RGB565 LE, 96x96, raw) para el launcher.
Uso: mkicon565.py entrada.{jpg,png} salida.icon565.bin [ancho alto]
Por defecto 96x96. Requiere ImageMagick (convert/magick)."""
import subprocess, struct, sys
src = sys.argv[1]; dst = sys.argv[2]
w = int(sys.argv[3]) if len(sys.argv) > 3 else 96
h = int(sys.argv[4]) if len(sys.argv) > 4 else 96
cmd = ['convert', src, '-resize', f'{w}x{h}!', '-depth', '8', 'rgb:-']
try:
    rgb = subprocess.check_output(cmd)
except Exception:
    rgb = subprocess.check_output(['magick', 'convert'] + cmd[1:])
out = bytearray()
for i in range(0, w*h*3, 3):
    r,g,b = rgb[i], rgb[i+1], rgb[i+2]
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    out += struct.pack('<H', v)
open(dst,'wb').write(out)
print(dst, len(out))
