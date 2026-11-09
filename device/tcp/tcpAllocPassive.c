/**
 * @file tcpAllocPassive.c
 *
 * Allocate a TCP device for a passive (listening) socket.
 *
 * A listening socket has no four-tuple, so the state of a socket that is
 * only waiting out its TIME_WAIT (or the remainder of a half-close, which
 * is what an HTTP connection leaves behind when the client disappears
 * after acknowledging our FIN) is irrelevant to it.  On a small TCB
 * table, those lingering sockets would otherwise starve a server that
 * closes the connection after every request: the table fills up and the
 * server cannot listen again until the timer purges the oldest entry.
 * This allocator reclaims such a socket before giving up.
 */
/* Embedded Xinu, ported to ESP32-S3. */

#include <stddef.h>
#include <semaphore.h>
#include <tcp.h>

/**
 * @ingroup tcp
 *
 * Allocate a tcp device, reclaiming one that is lingering in TIME_WAIT
 * when the free list is empty.
 * @return device number for a tcp device, SYSERR if none can be freed
 */
ushort tcpAllocPassive(void)
{
    ushort dev;
    int i;

    dev = tcpAlloc();
    if (SYSERR != (short)dev)
    {
        return dev;
    }

    for (i = 0; i < NTCP; i++)
    {
        wait(tcptab[i].mutex);
        if (((TCP_TIMEWT == tcptab[i].state)
             || (TCP_FINWT2 == tcptab[i].state)
             || (TCP_CLOSING == tcptab[i].state))
            && (TCP_ALLOC == tcptab[i].devstate))
        {
            /* tcpFree releases the mutex itself. */
            tcpFree(&tcptab[i]);
            return tcpAlloc();
        }
        signal(tcptab[i].mutex);
    }

    return SYSERR;
}
