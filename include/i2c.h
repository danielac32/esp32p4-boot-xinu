/**
 * @file i2c.h
 *
 * I2C0 master bus device.  The raw driver lives in drivers/i2c/; this
 * device serializes access to the shared bus (touch + ES8311 + AXP2101)
 * with a monitor and exposes transactions through control().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _I2C_H_
#define _I2C_H_

#include <device.h>
#include <device/i2c/i2c.h>

/* control(I2C0, CMD, slave_addr7, arg) */
#define I2C_CTRL_PROBE       0x01 /**< arg1=addr7           -> OK if found     */
#define I2C_CTRL_WRITE       0x02 /**< arg1=addr7 arg2=&req -> i2c_write       */
#define I2C_CTRL_READ        0x03 /**< arg1=addr7 arg2=&req -> i2c_read        */
#define I2C_CTRL_WRITE_REG8  0x04 /**< arg1=addr7 arg2=&req -> i2c_write_reg8  */
#define I2C_CTRL_READ_REGS   0x05 /**< arg1=addr7 arg2=&req -> i2c_read_regs   */
#define I2C_CTRL_WRITE_REG16 0x06 /**< arg1=addr7 arg2=&req -> i2c_write_reg16 */
#define I2C_CTRL_READ_REGS16 0x07 /**< arg1=addr7 arg2=&req -> i2c_read_regs16 */

/**
 * I2C transaction request (addressed to a 7-bit slave).
 * For *_REG* commands @c reg is the register; for a write @c buf holds the
 * value(s), for a read the data is returned in @c buf (length @c len).
 */
struct i2c_req
{
    uint16_t reg;               /**< register number (8- or 16-bit ops)  */
    uint8_t *buf;               /**< data buffer                         */
    size_t len;                 /**< data length                         */
};

/* Driver functions */
devcall i2cInit(device *);
devcall i2cControl(device *, int, long, long);

/* Serialize raw access to the shared I2C0 bus.  Single transactions do not
 * need these: control(I2C0, ...) takes the bus monitor itself, and that is
 * the path on-bus devices (touch, ES8311) use.  Keep these hooks only for a
 * sequence of transactions that must not interleave with another user's; the
 * monitor is recursive per thread, so control() still works inside a held
 * region. */
void i2cBusLock(void);
void i2cBusUnlock(void);

#endif                          /* _I2C_H_ */
