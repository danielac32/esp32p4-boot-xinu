/*
 * native_net.c — natives de esp32p4.Net para el ESP32-P4.
 *
 * Mínimo por ahora: la IPv4 de la interfaz y el estado del enlace.  ETH0 es
 * el ESP32-C6 presentado como netif por device/espeth-esp32p4, así que el
 * enlace no se lee de un registro PHY (como el W5500 del S3) sino de
 * espethLinkIsUp(), que es el AND del transporte SDIO montado y la asociación
 * STA del C6.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <device.h>
#include <monitor.h>
#include <network.h>
#include <espeth.h>

#include "simple_jvm.h"
#include "java_lib.h"
#include "jvm_io.h"

/* esp32p4.Net.ip(): la IPv4 de la interfaz, como String Java. */
int esp32p4_net_ip(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    struct netif *iface = netLookup(ETH0);
    char buf[32];

    (void)p;
    (void)type;
    if (iface != NULL)
    {
        netaddrsprintf(buf, &iface->ip);
    }
    else
    {
        strcpy(buf, "0.0.0.0");
    }
    pushObj(stack, heap_new_string(buf));
    return 0;
}

/* esp32p4.Net.isConnected(): 1 cuando el C6 tiene enlace. */
int esp32p4_net_isConnected(StackFrame *stack, SimpleConstantPool *p,
                            char *type)
{
    (void)p;
    (void)type;
    pushInt(stack, espethLinkIsUp() ? 1 : 0);
    return 0;
}
