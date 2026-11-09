#!/usr/bin/env python3
"""p4serial — consola serie de la placa: resetea y escucha/manda comandos.

Uso:  python3 p4serial.py <comando> [intervalo-s] [total-s]

Abre /dev/ttyACM0 (exclusivo: corta cualquier telnet abierto y saca la placa de
reset), deja arrancar el kernel y cada `intervalo-s' segundos escribe `comando'
por la shell, vomitando todo lo que sale con marca de tiempo desde el reset.

Con `comando' vacio solo escucha.  Ejemplos:
  python3 p4serial.py "" 0 60              escucha el arranque
  python3 p4serial.py "gpio read 28" 15 300    el nivel del backlight, 5 minutos
"""
import sys, time
import serial

PORT, BAUD = "/dev/ttyACM0", 115200
ARRANQUE = 4


def main(cmd, intervalo, total):
    ser = serial.Serial(PORT, BAUD, timeout=0.5, exclusive=True)
    t0 = time.time()

    def escuchar(dur):
        fin = time.time() + dur
        while time.time() < fin:
            linea = ser.readline()
            if linea:
                print("%6.1f| %s" % (time.time() - t0,
                      linea.decode("utf-8", "replace").rstrip()), flush=True)

    ser.reset_input_buffer()
    print("%6.1f| puerto abierto (la placa se esta reseteando)" % 0.0, flush=True)
    escuchar(ARRANQUE)

    if cmd:
        restantes = total - ARRANQUE
        while restantes > 0:
            ser.write((cmd + "\r\n").encode())
            escuchar(min(intervalo, max(restantes, 1)))
            restantes -= intervalo
    else:
        escuchar(total - ARRANQUE)
    ser.close()


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    paso = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
    todo = float(sys.argv[3]) if len(sys.argv) > 3 else 180.0
    main(cmd, paso, todo)
