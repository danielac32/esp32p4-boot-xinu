/**
 * @file net.h
 *
 * Network device (WiFi via the ESP32-C6 coprocessor).  The real bring-up
 * (esp-hosted SDIO attach + DHCP + lwIP pump) needs the scheduler and ticks,
 * so it does NOT run here: netinit() only registers the device (no-op, like
 * the reference SDK).  The WiFi bring-up runs in the NET thread (nettask.c),
 * created from main().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _NET_H_
#define _NET_H_

#include <device.h>
#include <thread.h>

/* netControl() commands: control(NET0, CMD, 0, 0) */
#define NET_CTRL_HAS_IP   0x01 /**< returns 1 if the netif has an IP     */
#define NET_CTRL_IP       0x02 /**< arg2 = uint32_t* to receive the IP   */

/* Device functions */
devcall netinit(device *);
devcall netControl(device *, int, long, long);

/* WiFi bring-up + pump task (started by main). */
thread netTask(void);

#endif                          /* _NET_H_ */
