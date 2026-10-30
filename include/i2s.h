/**
 * @file i2s.h
 *
 * I2S0 audio device.  The raw driver lives in drivers/i2s/; this device
 * serializes access (playback + capture share the GDMA engine and the
 * controller) with a monitor and exposes it through control().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _I2S_H_
#define _I2S_H_

#include <device.h>
#include <drivers/i2s/i2s.h>

/* control(I2S0, CMD, arg1, arg2) */
#define I2S_CTRL_BEEP    0x01 /**< arg1=freq_hz  arg2=ms              */
#define I2S_CTRL_PLAY    0x02 /**< arg1=(const int16_t*)frames arg2=frames (stereo) */
#define I2S_CTRL_RECORD  0x03 /**< arg1=(int16_t*)buf arg2=samples (stereo)         */

/* Driver functions */
devcall i2sInit(device *);
devcall i2sControl(device *, int, long, long);

#endif                          /* _I2S_H_ */
