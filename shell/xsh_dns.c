/**
 * @file xsh_dns.c
 *
 * Resolver DNS minimo y el comando `dns' de la shell.
 *
 * El servidor va fijo a 8.8.8.8 porque este port no mira la opcion 6 de la
 * respuesta DHCP (network/dhcpc no parsea servidores DNS); se pregunta por el
 * UDP0 y la interfaz ETH0, que es la que sube `netup'.  Si ya no hay ruta por
 * el C6 el read() expira y el comando devuelve fallo, sin colgar.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <shell.h>
#include <device.h>
#include <network.h>
#include <udp.h>

int dns_resolve(const char *host, struct netaddr *out)
{
    struct netaddr dns_srv, localip;
    struct netif *iface;
    int udpdev;
    unsigned char qbuf[256];
    static unsigned char rbuf[512];
    int qlen;
    int rlen;
    const char *p;

    /* Already a dotted quad?  Try that first. */
    if (dot2ipv4(host, out) == OK)
        return OK;

    /* ETH0 por nombre: getdev() lee el devtab, asi que el comando no depende
     * del numero que le toque a la interfaz en el conf. */
    iface = netLookup(getdev("ETH0"));
    if (!iface)
        return SYSERR;
    netaddrcpy(&localip, &iface->ip);

    dns_srv.type = NETADDR_IPv4;
    dns_srv.len = 4;
    dns_srv.addr[0] = 8;
    dns_srv.addr[1] = 8;
    dns_srv.addr[2] = 8;
    dns_srv.addr[3] = 8;

    udpdev = UDP0;
    if (open(udpdev, &localip, &dns_srv, 0, 53) != OK)
    {
        printf("dns: no se pudo abrir el UDP0\n");
        return SYSERR;
    }

    /* Header: ID, flags (standard query, recursion desired), QDCOUNT=1. */
    qbuf[0] = 0x12;
    qbuf[1] = 0x34;
    qbuf[2] = 0x01;
    qbuf[3] = 0x00;
    qbuf[4] = 0x00;
    qbuf[5] = 0x01;
    qbuf[6] = qbuf[7] = qbuf[8] = qbuf[9] = 0;
    qbuf[10] = qbuf[11] = 0;
    qlen = 12;

    /* Encode the hostname as QNAME. */
    p = host;
    while (*p)
    {
        const char *dot = strchr(p, '.');
        int lablen = dot ? (int)(dot - p) : (int)strlen(p);

        if (lablen <= 0 || lablen > 63 || qlen + 1 + lablen > 240)
            break;
        qbuf[qlen++] = (unsigned char)lablen;
        memcpy(qbuf + qlen, p, lablen);
        qlen += lablen;
        p += lablen;
        if (*p == '.')
            p++;
    }
    qbuf[qlen++] = 0;
    qbuf[qlen++] = 0x00;
    qbuf[qlen++] = 0x01;            /* QTYPE = A  */
    qbuf[qlen++] = 0x00;
    qbuf[qlen++] = 0x01;            /* QCLASS = IN */

    if (write(udpdev, qbuf, qlen) != qlen)
    {
        printf("dns: fallo el envio de la consulta (%d bytes)\n", qlen);
        close(udpdev);
        return SYSERR;
    }

    rlen = read(udpdev, rbuf, sizeof(rbuf));
    close(udpdev);
    if (rlen < 12)
    {
        printf("dns: sin respuesta del servidor (read devolvio %d)\n", rlen);
        return SYSERR;
    }

    if (((rbuf[6] << 8) | rbuf[7]) < 1)
    {
        printf("dns: id=%02x%02x flags=%02x%02x ANCOUNT=%u\n", rbuf[0],
               rbuf[1], rbuf[2], rbuf[3],
               (unsigned)((rbuf[6] << 8) | rbuf[7]));
        return SYSERR;
    }

    /* Skip the question section. */
    {
        int off = 12;

        while (off < rlen && rbuf[off] != 0)
        {
            if ((rbuf[off] & 0xC0) == 0xC0)
            {
                off += 2;
                break;
            }
            off += 1 + rbuf[off];
        }
        if (off < rlen && rbuf[off] == 0)
            off++;
        off += 4;

        /* Walk the answers looking for an A record. */
        while (off + 12 <= rlen)
        {
            int type, cls, rdlen;

            if ((rbuf[off] & 0xC0) == 0xC0)
                off += 2;
            else
            {
                while (off < rlen && rbuf[off] != 0)
                    off += 1 + rbuf[off];
                off++;
            }
            if (off + 10 > rlen)
                break;
            type = (rbuf[off] << 8) | rbuf[off + 1];
            off += 2;
            cls = (rbuf[off] << 8) | rbuf[off + 1];
            off += 2;
            off += 4;               /* TTL */
            rdlen = (rbuf[off] << 8) | rbuf[off + 1];
            off += 2;
            if (off + rdlen > rlen)
                break;
            if (type == 1 && cls == 1 && rdlen == 4)
            {
                out->type = NETADDR_IPv4;
                out->len = 4;
                memcpy(out->addr, rbuf + off, 4);
                return OK;
            }
            off += rdlen;
        }
    }

    return SYSERR;
}

shellcmd xsh_dns(int nargs, char *args[])
{
    struct netaddr ip;
    char buf[24];

    if (nargs < 2)
    {
        printf("Usage: dns <hostname>\n");
        return SHELL_ERROR;
    }
    if (dns_resolve(args[1], &ip) != OK)
    {
        printf("dns: failed to resolve '%s'\n", args[1]);
        return SHELL_ERROR;
    }
    netaddrsprintf(buf, &ip);
    printf("%s -> %s\n", args[1], buf);
    return SHELL_OK;
}
