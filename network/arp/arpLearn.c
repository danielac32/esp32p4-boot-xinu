/**
 * @file arpLearn.c
 *
 * Learn (or refresh) an ARP entry from the sender addresses of an inbound
 * IPv4 frame.  Cheap, standard ARP-cache behaviour, and it also avoids a
 * deadlock: arpLookup() blocks the calling thread while it waits for the
 * ARP reply, but the only thread that reads replies off the wire is the
 * network receive thread itself, so a SYN-ACK to a host that is not yet
 * in the table could never be sent.  Learning the entry from the very
 * frame that triggered the reply removes that round trip.
 */
/* Embedded Xinu, ported to ESP32-S3. */

#include <stddef.h>
#include <arp.h>
#include <clock.h>
#include <interrupt.h>
#include <ipv4.h>
#include <network.h>
#include <string.h>

/**
 * @ingroup arp
 *
 * Record that (praddr, hwaddr) belong together.  Existing entries are
 * refreshed, unresolved entries are completed, and new ones are created.
 *
 * @param netptr network interface the frame arrived on
 * @param praddr sender protocol address
 * @param hwaddr sender hardware address
 * @return OK if the entry was recorded, SYSERR otherwise
 */
syscall arpLearn(struct netif *netptr, const struct netaddr *praddr,
                 const struct netaddr *hwaddr)
{
    struct arpEntry *entry = NULL;
    irqmask im;
    int i;
    int unresolved;

    if ((NULL == netptr) || (NULL == praddr) || (NULL == hwaddr))
    {
        return SYSERR;
    }
    if ((NETADDR_IPv4 != praddr->type) ||
        (NETADDR_ETHERNET != hwaddr->type) ||
        (IPv4_ADDR_LEN != praddr->len) ||
        (ETH_ADDR_LEN != hwaddr->len))
    {
        return SYSERR;
    }

    /* Ignore unnamed senders and group/broadcast hardware addresses. */
    for (i = 0; i < IPv4_ADDR_LEN; i++)
    {
        if (praddr->addr[i] != 0)
        {
            break;
        }
    }
    if (i == IPv4_ADDR_LEN)
    {
        return SYSERR;
    }
    if (hwaddr->addr[0] & 0x01)
    {
        return SYSERR;
    }

    im = disable();
    entry = arpGetEntry(praddr);
    unresolved = (NULL != entry) && (ARP_RESOLVED != entry->state);

    if (NULL == entry)
    {
        entry = arpAlloc();
        if (SYSERR == (int)entry)
        {
            restore(im);
            return SYSERR;
        }
        entry->state = ARP_RESOLVED;
        entry->nif = netptr;
        netaddrcpy(&entry->praddr, praddr);
        netaddrcpy(&entry->hwaddr, hwaddr);
        entry->expires = clktime + ARP_TTL_RESOLVED;
        entry->count = 0;
    }
    else
    {
        entry->state = ARP_RESOLVED;
        entry->nif = netptr;
        netaddrcpy(&entry->hwaddr, hwaddr);
        entry->expires = clktime + ARP_TTL_RESOLVED;
    }
    restore(im);

    /* Wake anything that was blocked waiting for this address. */
    if (unresolved)
    {
        arpNotify(entry, 0);
    }

    return OK;
}
