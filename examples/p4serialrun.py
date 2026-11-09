#!/usr/bin/env python3
"""p4serialrun — secuencia de comandos por la consola serie de la placa.

Uso:  python3 p4serialrun.py <passo-obertura-s> "comando|espera-s" ...

Abre /dev/ttyACM0 (exclusivo: corta el telnet y saca la placa de reset), deja
arrancar el kernel y va mandando los pasos que le dan, escuchando todo lo que
sale con marca de tiempo desde el reset.

Cada paso es "comando|segundos-a-esperar".  Un paso con comando vacio es solo
espera.  Los "java /java/X.class" bloquean la shell hasta que muere la hebra de
la app, asi que su espera tiene que cubrir toda la ejecucion.

Ejemplo:
  python3 p4serialrun.py 25 ""|20 "java /java/NatTest.class|8"
"""
import sys, time
import serial

PORT, BAUD = "/dev/ttyACM0", 115200
ARRANQUE = 25


def escuchar(ser, t0, dur):
    fin = time.time() + dur
    while time.time() < fin:
        linea = ser.readline()
        if linea:
            print("%6.1f| %s" % (time.time() - t0,
                  linea.decode("utf-8", "replace").rstrip()), flush=True)


def main(paso_arranque, pasos):
    ser = serial.Serial(PORT, BAUD, timeout=0.5, exclusive=True)
    t0 = time.time()

    ser.reset_input_buffer()
    print("%6.1f| puerto abierto (la placa se esta reseteando)" % 0.0, flush=True)
    escuchar(ser, t0, max(paso_arranque, ARRANQUE))

    for paso in pasos:
        cmd, _, espera = paso.partition("|")
        escuchar(ser, t0, 1.0)                # deja terminar la linea anterior
        if cmd:
            print("%6.1f| >> %s" % (time.time() - t0, cmd), flush=True)
            ser.write((cmd + "\r\n").encode())
        escuchar(ser, t0, float(espera) if espera else 5.0)
    ser.close()


if __name__ == "__main__":
    primero = sys.argv[1] if len(sys.argv) > 1 else "25"
    if primero.isdigit():
        main(float(primero), sys.argv[2:])
    else:
        main(25.0, sys.argv[1:])
