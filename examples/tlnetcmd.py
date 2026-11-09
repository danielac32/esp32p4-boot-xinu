#!/usr/bin/env python3
"""tlnetcmd — manda comandos de shell a la placa por telnet y devuelve la salida.

Uso:  python3 tlnetcmd.py <segundos-de-espera> "cmd1" "cmd2" ...

Abre una sesion TCP al servidor telnet del kernel (device/telnet, puerto 23),
espera `segundos-de-espera' por comando y vomita todo lo recibido.  La pantalla
no se ve desde aqui: lo que se comprueba es el stdout de la app (los System.out
con los numeros leidos del kernel).  La parte visual necesita dedo en la placa.
"""
import socket, sys, time

HOST, PORT = "192.168.1.146", 23


def drain(sock, estado, segundos):
    fin = time.time() + segundos
    while time.time() < fin:
        try:
            d = sock.recv(4096)
        except (socket.timeout, OSError):
            continue
        if not d:
            return
        estado[0] += d


def run(cmds, espera):
    sock = socket.create_connection((HOST, PORT), timeout=10)
    sock.settimeout(1.0)
    estado = [b""]

    drain(sock, estado, 2.0)                 # banner + prompt
    for c in cmds:
        sock.sendall((c + "\r\n").encode())
        drain(sock, estado, espera)
    sock.close()
    return estado[0].decode("utf-8", "replace").replace("\r\n", "\n")


if __name__ == "__main__":
    print(run(sys.argv[2:], float(sys.argv[1])))
