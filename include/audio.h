/**
 * @file audio.h
 *
 * Audio codec device (ES8311 on I2C0).  The raw driver lives in
 * drivers/audio/; this device brings it up and serializes codec register
 * access with the shared I2C bus monitor (owned by device/i2c).  The PCM
 * stream itself is on the I2S0 device (see <i2s.h>).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _AUDIO_H_
#define _AUDIO_H_

#include <device.h>
#include <drivers/audio/es8311.h>

/* control(AUDIO0, CMD, arg1, arg2) */
#define AUDIO_CTRL_VOLUME    0x01 /**< arg1=0..100                       */
#define AUDIO_CTRL_MUTE      0x02 /**< arg1=0/1                          */
#define AUDIO_CTRL_POWERDOWN 0x03 /**< apaga analógico + DAC             */

/* Driver functions */
devcall audioInit(device *);
devcall audioControl(device *, int, long, long);

#endif                          /* _AUDIO_H_ */
