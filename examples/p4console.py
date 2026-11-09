#!/usr/bin/env python3
"""p4console -- consola serie VIVA y reutilizable, sin resetear entre comandos.

Uso:  python3 p4console.py            (deja un FIFO en /tmp/p4cmd-<pid>.fifo)

Abrir /dev/ttyACM0 NO resetea la placa (medido: el 'xsh$' aparece a los 2 s y no
hay banner), pero si la sostiene otro proceso el puerto esta ocupado; por eso
esta herramienta se abre UNA vez, espera el 'xsh$' y ya no suelta el puerto: los
comandos se escriben en el FIFO y las lineas de la placa salen a stdout con marca
de tiempo.  Eso es lo que falta en p4shellwait.py: lanzar una app que no retorna
desde telnet y, MIENTRAS vive, preguntarle el censo a la shell serie.

El FIFO lleva el pid en el nombre: con uno fijo, un segundo arranque borraba la
tubería que el primero estaba esperando con open(), y desde entonces ningún
comando llegaba a la placa -- escribiente y lector esperaban inodos distintos.

Los comandos admiten la forma 'comando|segundos' para darle a la placa la
ventana de lectura por la que ya se espera.
"""
import os
import sys
import threading
import time

import serial

PORT, BAUD = "/dev/ttyACM0", 115200
FIFO = "/tmp/p4cmd-%d.fifo" % os.getpid()
MAX_ESPERA = 420            # s de cortocircuito: netboot puede ir reintentando


class Consola:
    def __init__(self, ser, t0):
        self.ser = ser
        self.t0 = t0
        self.traba = threading.Lock()   # protege el puerto Y el historial
        self.historial = ""

    def linea(self, s):
        sys.stdout.write("%6.1f| %s\n" % (time.time() - self.t0, s))
        sys.stdout.flush()

    def leer(self):
        """Unico lector del puerto: si leen dos, se reparten lineas sueltas."""
        while True:
            linea = self.ser.readline()
            if not linea:
                continue
            s = linea.decode("utf-8", "replace").rstrip()
            with self.traba:
                self.historial = (self.historial + " " + s)[-4000:]
            self.linea(s)

    def mandar(self, cmd):
        with self.traba:
            if cmd:
                self.linea(">> %s" % cmd)
            self.ser.write((cmd + "\n").encode())
            self.ser.flush()

    def ha_visto(self, aguja):
        with self.traba:
            return aguja in self.historial


def main():
    if os.path.exists(FIFO):
        os.remove(FIFO)
    os.mkfifo(FIFO)

    ser = serial.Serial(PORT, BAUD, timeout=0.5, exclusive=True)
    t0 = time.time()
    ser.reset_input_buffer()
    print("%6.1f| puerto abierto (abrirlo no resetea: el 'xsh$' tardara lo que "
          "tarde el arranque en curso)" % 0.0, flush=True)

    c = Consola(ser, t0)
    threading.Thread(target=c.leer, daemon=True).start()

    # El prompt no se imprime solo: se le pide mandando retornos hasta que la
    # shell devuelva un 'xsh$' que no sea eco de otra cosa.
    while time.time() - t0 < MAX_ESPERA:
        time.sleep(2.0)
        c.mandar("")
        if c.ha_visto("xsh$"):
            time.sleep(1.0)
            c.mandar("")
            if c.ha_visto("xsh$"):
                break

    if not c.ha_visto("xsh$"):
        print("!! no hubo xsh$ en %d s" % MAX_ESPERA, flush=True)
        return 1
    print("%6.1f| consola lista (comandos: echo 'ls|3' > %s)"
          % (time.time() - t0, FIFO), flush=True)

    while True:
        try:
            fd = os.open(FIFO, os.O_RDONLY)            # bloquea hasta que hay quien escriba
        except OSError:
            time.sleep(1)
            continue
        with os.fdopen(fd, "r") as f:
            for raw in f:
                paso = raw.strip()
                if not paso:
                    continue
                cmd, _, espera = paso.partition("|")
                c.mandar(cmd)
                time.sleep(float(espera or 2))
        time.sleep(0.2)


if __name__ == "__main__":
    sys.exit(main())
