/**
 * @file i2cControl.c
 *
 * The `i2c` device: control() dispatcher over the shared I2C0 bus.
 * See include/i2c.h for the command codes (control(I2C0, CMD, slave, req)).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <monitor.h>
#include <i2c.h>

/* Monitor owned by the i2c device (created in i2cInit.c). */
extern monitor i2cmon;

/**
 * @ingroup i2c
 *
 * Control an I2C transaction.
 *
 * @param devptr I2C device table entry
 * @param func   command (I2C_CTRL_*)
 * @param arg1   7-bit slave address
 * @param arg2   command-dependent (usually a struct i2c_req *)
 * @return OK/SYSERR
 */
devcall i2cControl(device *devptr, int func, long arg1, long arg2)
{
    uint8_t dev = (uint8_t) arg1;
    struct i2c_req *req = (struct i2c_req *) arg2;
    bool ok;

    (void)devptr;

    /*if (SYSERR == i2cmon)
    {
        return SYSERR;
    }*/

    //lock(i2cmon);
    switch (func)
    {
    case I2C_CTRL_PROBE:
        ok = i2c_probe(dev);
        break;

    case I2C_CTRL_WRITE:
        ok = req && i2c_write(dev, req->buf, req->len);
        break;

    case I2C_CTRL_READ:
        ok = req && i2c_read(dev, req->buf, req->len);
        break;

    case I2C_CTRL_WRITE_REG8:
        ok = req && req->buf && i2c_write_reg8(dev, (uint8_t) req->reg,
                                               req->buf[0]);
        break;

    case I2C_CTRL_READ_REGS:
        ok = req && i2c_read_regs(dev, (uint8_t) req->reg, req->buf, req->len);
        break;

    case I2C_CTRL_WRITE_REG16:
        ok = req && req->buf && i2c_write_reg16(dev, req->reg, req->buf[0]);
        break;

    case I2C_CTRL_READ_REGS16:
        ok = req && i2c_read_regs16(dev, req->reg, req->buf, req->len);
        break;

    default:
        ok = FALSE;
        break;
    }
    //unlock(i2cmon);

    return ok ? OK : SYSERR;
}
