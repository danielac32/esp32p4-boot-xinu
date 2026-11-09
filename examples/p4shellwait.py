#!/usr/bin/env python3
"""p4shellwait -- espera el 'xsh$' de la consola y entonces manda los pasos.

Uso:  python3 p4shellwait.py "comando|espera-s" ...

Los esperas fijas de p4serialrun.py se pasan de cortas o de largas cuando el
gate de netboot del arranque esta reintentando la AP grabada (5 intentos de
~50 s).  Aqui se abre el puerto (lo que resetea la placa), se le hasta que
aparece el prompt y despues se ejecutan los pasos.
"""
import sys, time
import serial

PORT, BAUD = "/dev/ttyACM0", 115200
MAX_ESPERA = 420          # s de cortocircuito: netboot puede ir reintentando


def oir(ser, t0, dur):
    fin = time.time() + dur
    salidas = []
    while time.time() < fin:
        linea = ser.readline()
        if linea:
            s = linea.decode("utf-8", "replace").rstrip()
            print("%6.1f| %s" % (time.time() - t0, s), flush=True)
            salidas.append(s)
    return salidas


def main(pasos):
    ser = serial.Serial(PORT, BAUD, timeout=0.5, exclusive=True)
    t0 = time.time()
    ser.reset_input_buffer()
    print("%6.1f| puerto abierto" % 0.0, flush=True)

    visto = ""
    while time.time() - t0 < MAX_ESPERA:
        linea = ser.readline()
        if not linea:
            continue
        s = linea.decode("utf-8", "replace").rstrip()
        print("%6.1f| %s" % (time.time() - t0, s), flush=True)
        visto = (visto + s)[-200:]
        if "xsh$" in s:
            break

    if "xsh$" not in visto:
        print("!! no habio xsh$ en %d s" % MAX_ESPERA, flush=True)
        return 1

    # El prompt se lee pegado a lo que llega despues, asi que el primer "xsh$"
    # puede ser el eco de una linea que todavia no es nuestra: se confirma
    # mandando un retorno y esperando que la shell lo devuelva solo.
    ser.write(b"\n")
    t_conf = time.time()
    while time.time() - t_conf < 20:
        linea = ser.readline()
        if linea:
            s = linea.decode("utf-8", "replace").rstrip()
            print("%6.1f| %s" % (time.time() - t0, s), flush=True)
            if "xsh$" in s and "netboot" not in s and "wifi" not in s:
                break
        else:
            ser.write(b"\n")

    print("%6.1f| consola lista" % (time.time() - t0), flush=True)
    for paso in pasos:
        cmd, _, espera = paso.partition("|")
        oir(ser, t0, 0.6)
        if cmd:
            print("%6.1f| >> %s" % (time.time() - t0, cmd), flush=True)
            ser.write((cmd + "\n").encode())
            ser.flush()
        oir(ser, t0, float(espera or 3))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
