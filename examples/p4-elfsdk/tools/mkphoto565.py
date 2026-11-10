#!/usr/bin/env python3
"""Convierte imagen a .565 raw RGB565 LE (por defecto 320x240, ajusta manteniendo aspecto sobre fondo negro)."""
import subprocess, struct, sys, os
src, dst = sys.argv[1], sys.argv[2]
w = int(sys.argv[3]) if len(sys.argv)>3 else 320
h = int(sys.argv[4]) if len(sys.argv)>4 else 240
cmd = ['convert', src, '-auto-orient', '-resize', f'{w}x{h}', '-background', 'black',
       '-gravity', 'center', '-extent', f'{w}x{h}', '-depth', '8', 'rgb:-']
try: rgb = subprocess.check_output(cmd, stderr=subprocess.DEVNULL)
except Exception:
    try: rgb = subprocess.check_output(['magick']+cmd, stderr=subprocess.DEVNULL)
    except Exception: sys.exit(1)
out = bytearray()
for i in range(0, w*h*3, 3):
    r,g,b = rgb[i], rgb[i+1], rgb[i+2]
    out += struct.pack('<H', ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3))
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst,'wb').write(out)
