#!/usr/bin/env python3
"""Referencia EXTERNA del reloj del ESP32-P4.

El unico reloj absoluto que hay en la placa es el cristal que genera el mtime;
compararlo contra si mismo no dice nada.  Este script es la otra mitad:
escucha el latido que suelta `syscheck clk` por telnet y mide cuanto tarda el
PC en verlo.

Que contesta:

  ratio   = ms_del_kernel / ms_del_PC
            1,000 -> el reloj del kernel y el del PC estan de acuerdo
            1,0xx -> el kernel corre adelgazado o el clkfreq declarado no es
                     el del cristal
  ppm     = desvio en partes por millon
  deriva  = segundos ganados o perdidos por hora del kernel

Uso:

    python3 p4clk.py <ip-de-la-placa> [lineas]

Requisitos: la placa con el servidor telnet arrancado (`telnetd`, puerto 23) y
el `syscheck` compilado dentro de la shell.  No toca /dev/ttyACM0, asi que no
resetea la placa: es la via comoda para medir sin perder el estado del WiFi.

Nota de metodo: el latido sale de `sleep(1000)`, o sea del propio reloj que se
quiere medir, y el PC lo recibe por TCP con la shell imprimiendo por delante.
Por eso se miran la PENDIENTE y la MEDIANA de los intervalos, no el primer
retardo: el primer HB lleva el coste de abrir la conexion y de arrancar el
comando.  Los saltos >1500 ms se cuentan aparte, porque son colas de la shell,
no reloj.
"""

import re
import select
import socket
import statistics
import sys
import time

HB = re.compile(r"^HB (\d+) xinums=(\d+) uptime=(\d+) mtime=(\d+)$")

IAC, DONT, DO, WILL, WONT = 0xFF, 0xFD, 0xFE, 0xFB, 0xFC


class Telnet:
    """Cliente telnet minimo: quita la negociacion IAC y entrega lineas.

    No hace falta un telnet completo: el servidor del port Xinu negocia poco y
    lo que nos interesa es el texto.  Las opciones se responden con WONT/DONT
    para que el otro lado no insista.
    """

    def __init__(self, host, port=23):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.setblocking(False)
        self.buf = bytearray()
        self.partial = ""
        self.done = []            # (linea, instante_PC de llegada de la trama)

    def pump(self, deadline):
        while time.monotonic() < deadline:
            r, _, _ = select.select([self.s], [], [], 0.05)
            if not r:
                continue
            try:
                data = self.s.recv(4096)
                stamp = time.monotonic()
            except BlockingIOError:
                continue
            if not data:
                return False
            self._drain(data, stamp)
        return True

    def _drain(self, data, stamp):
        out = bytearray()
        self.buf.extend(data)
        i = 0
        b = self.buf
        while i < len(b):
            if b[i] == IAC and i + 2 < len(b):
                cmd, opt = b[i + 1], b[i + 2]
                if cmd in (DO, DONT):
                    self._sendaback(WONT, opt)
                elif cmd in (WILL, WONT):
                    self._sendaback(DONT, opt)
                i += 3
                continue
            out.append(b[i])
            i += 1
        del self.buf[:]
        text = out.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
        parts = text.split("\n")
        parts[0] = self.partial + parts[0]
        # Cada linea lleva el instante en que LLEGO su trama, no cuando se lee
        # el lote: si no, los 30 latidos salen temporalizados en el mismo
        # microsegundo y el ratio contra el PC no significa nada.
        for ln in parts[:-1]:
            self.done.append((ln, stamp))
        self.partial = parts[-1]

    def _sendaback(self, cmd, opt):
        try:
            self.s.setblocking(True)
            self.s.sendall(bytes([IAC, cmd, opt]))
            self.s.setblocking(False)
        except OSError:
            pass

    def take(self):
        """Devuelve y vacia las lineas completas acumuladas, con su instante."""
        done = self.done
        self.done = []
        return done


def main():
    if len(sys.argv) < 2 or "--help" in sys.argv:
        print(__doc__)
        return 0
    host = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 30

    t = Telnet(host)
    t.pump(time.monotonic() + 1.0)
    t.take()
    t.s.setblocking(True)
    t.s.sendall(b"syscheck clk\r\n")
    t.s.setblocking(False)

    print("esperando %d latidos de %s ..." % (want, host))
    # (instante_PC, xinums, uptime, mtime)
    samples = []
    deadline = time.monotonic() + 60.0 + want * 1.2
    while len(samples) < want and time.monotonic() < deadline:
        if not t.pump(deadline):
            print("conexion cerrada por la placa")
            break
        for ln, stamp in t.take():
            m = HB.match(ln.strip())
            if m:
                # `stamp` es el instante de llegada de la trama que trajo esa
                # linea: es lo que se compara con el xinums que la placa escribio
                # al emitirla.  El retardo de la red es constante y se va con la
                # pendiente; lo que no se puede es temporalizar el lote entero.
                samples.append((stamp, int(m.group(2)),
                                int(m.group(3)), int(m.group(4))))
                sys.stdout.write(".")
                sys.stdout.flush()
    t.s.close()
    print()

    if len(samples) < 4:
        print("no hay bastante muestra (%d latidos)" % len(samples))
        return 1

    t0, m0 = samples[0][0], samples[0][1]
    t1, m1 = samples[-1][0], samples[-1][1]
    pc_ms = (t1 - t0) * 1000.0
    kn_ms = float(m1 - m0)
    ratio = kn_ms / pc_ms if pc_ms > 0 else 0.0
    ppm = (ratio - 1.0) * 1e6
    drift = (ratio - 1.0) * 3600.0

    gaps = [(samples[i][0] - samples[i - 1][0]) * 1000.0 for i in range(1, len(samples))]
    kgaps = [samples[i][1] - samples[i - 1][1] for i in range(1, len(samples))]
    saltos = [g for g in gaps if g > 1500.0]

    print("latidos          : %d" % len(samples))
    print("reloj del kernel : %8.1f ms" % kn_ms)
    print("reloj del PC     : %8.1f ms" % pc_ms)
    print("ratio            : %8.5f" % ratio)
    print("desvio           : %+8.1f ppm  (%+.3f s/hora)" % (ppm, drift))
    print("intervalo PC     : mediana %.1f ms, min %.1f, max %.1f"
          % (statistics.median(gaps), min(gaps), max(gaps)))
    print("intervalo kernel : mediana %.0f ms" % statistics.median(kgaps))
    print("latidos con salto>1500 ms (colas de shell, no reloj): %d" % len(saltos))

    # El mtime lo cuentan las crudas del contador; si platform.clkfreq fuese
    # 360e6, 1 s del kernel = 360e6 cuentas.  Esto comprueba el clkfreq
    # DECLARADO contra el paso real, sin pasar por clkhandler().
    # Ojo: `mtime=%lu` es la palabra BAJA del mtime de 64 bits, que envuelve
    # cada 2^32/360e6 = 11,93 s.  Como el latido va a ~1/s, cada paso cabe de
    # sobra en una vuelta y la resta modular por paso es correcta; lo que NO
    # vale es restar el primero del ultimo.
    W32 = 1 << 32
    pasos = []
    for i in range(1, len(samples)):
        dmt = (samples[i][3] - samples[i - 1][3]) % W32
        dms = float(samples[i][1] - samples[i - 1][1])
        if dms > 900.0 and dms < 1100.0:
            pasos.append(dmt / dms)
    if len(pasos) >= 4:
        hz = statistics.median(pasos) * 1000.0
        print("cuentas de mtime por segundo del kernel: %.3f MHz (mediana de %d pasos)"
              % (hz / 1e6, len(pasos)))
        print("  (el port declara P4_TIMER_FREQ = 360 MHz; si esto sale")
        print("   distinto, o el cristal no es de 40 MHz o el divisor de")
        print("   la CLINT esta configurado en otra cosa)")

    # Y la cifra ABSOLUTA, que es la que falta para interpretar la de arriba:
    # cuentas de mtime entre dos latidos separados EXACTAMENTE 1000 ms de xinums,
    # divididas por el tiempo REAL del PC que los separo.  Esto dice a que
    # frecuencia cuenta el mtime de verdad, sin depender de que clkhandler()
    # acierte con el segundo.
    reales = []
    for i in range(1, len(samples)):
        dms = float(samples[i][1] - samples[i - 1][1])
        dpc = samples[i][0] - samples[i - 1][0]
        dmt = (samples[i][3] - samples[i - 1][3]) % W32
        if dms == 1000.0 and 0.8 < dpc < 1.3:
            reales.append(dmt / dpc)
    if len(reales) >= 4:
        hz_pc = statistics.median(reales)
        print("mtime contra el PC : %.3f MHz (%d pasos, mediana)"
              % (hz_pc / 1e6, len(reales)))
        print("  sobre los 360 MHz declarados: ratio %.4f" % (hz_pc / 360e6))

    if abs(ppm) < 200.0:
        print("\nVEREDICTO: millis() coherente con el PC dentro de 200 ppm.")
    elif abs(ppm) < 2000.0:
        print("\nVEREDICTO: desvio pequeño. Puede ser cristal, puede ser la")
        print("resolucion de 1 ms del latido contra un PC con jitter de red.")
    else:
        print("\nVEREDICTO: desvio grande. Revisar platform.clkfreq y")
        print("P4_TIMER_FREQ (include/esp32p4.h) y si clkhandler() pierde ticks.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
