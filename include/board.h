/**
 * @file board.h
 *
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 board definition: pin map.
 *
 * Verified against the official schematic and ESP-IDF v5.5
 * soc/sdmmc_pins.h (SLOT0 = microSD).  Nothing here is a register; it is
 * only the wiring, so drivers/ can include it freely.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _BOARD_H_
#define _BOARD_H_

#include <stdint.h>

/* ======================================================================
 * LCD (ST7796, 320x480, SPI2)
 * ====================================================================== */
#define LCD_MOSI            20
#define LCD_SCLK            21
#define LCD_CS              23
#define LCD_DC              26
#define LCD_RST             27
#define LCD_BL              28
#define LCD_WIDTH           320
#define LCD_HEIGHT          480
#define LCD_PIXEL_CLOCK     80000000

/* ======================================================================
 * I2C bus (GPIO 7/8) — shared by touch + audio codec + PMIC
 * ====================================================================== */
#define I2C_SDA             7
#define I2C_SCL             8

#define TOUCH_ADDR_FT6336   0x38    /* populated on released boards */
#define TOUCH_ADDR_GT911_A  0x5D    /* variant (probe, not populated) */
#define TOUCH_ADDR_GT911_B  0x14
#define ES8311_ADDR0        0x18    /* CE low  */
#define ES8311_ADDR1        0x19    /* CE high */
#define AXP2101_ADDR        0x34    /* PMIC    */

/* Touch FPC lines (esquema Waveshare).  TP_RST is active low and the module
 * brings no pull-up, so the pad stays low after reset and the FT6336 never
 * answers on 0x38 until the kernel drives it high. */
#define TP_RST              29
#define TP_INT              50

/* ======================================================================
 * SD card (SDMMC SLOT0, IOMUX pins — fixed function)
 * ====================================================================== */
#define SD_D0               39
#define SD_D1               40
#define SD_D2               41
#define SD_D3               42
#define SD_CMD              44
#define SD_CLK              43
#define SD_CD               SD_D3   /* card-detect tied to D3 */
/* The slot has no power-switch GPIO: its VDD rail is the on-chip LDO
 * output VO4 (PMU LDO channel 4), enabled by the SD driver.  (GPIO45 is
 * USB_DM, not an SD power gate; the Waveshare BSPs drive no such pin.) */

/* ======================================================================
 * I2S (audio, ES8311 + amp + mic)
 * ====================================================================== */
#define I2S_DOUT            9
#define I2S_WS              10
#define I2S_DIN             11  /* mic path (via ES8311 ADC) */
#define I2S_BCK             12
#define I2S_MCK             13
#define I2S_AMP_POWER       53

/* ======================================================================
 * ESP32-C6 coprocessor (WiFi6, SDMMC SLOT1 via GPIO matrix)
 *
 * Verified against the vendor schematic (Waveshare
 * ESP32-P4-WIFI6-Touch-LCD-3.5-schematic.pdf, U1 = ESP32-C6FH8): the C6's
 * SDIO-slave pads come out as package pins 24/23 (SDIO_CLK/SDIO_CMD) and
 * 29..26 (SDIO_DATA3..DATA0), and those wires are net-named GPIO18, GPIO19
 * and GPIO17..GPIO14 — i.e. the P4 pins below.  The slave's own IOMUX is a
 * contiguous block too (SDK soc/sdio_slave_pins.h: CMD=18, CLK=19, D0..D3 =
 * 20..23 on the C6), so the wiring is D0..D3 straight across, no swap.
 * DAT0..DAT3 additionally carry 51k pull-ups to ESP_3V3 (R15..R18) and the
 * reset/strap levers are series 0R (R54 -> CHIP_PU, R52 -> GPIO2), exactly
 * as assumed.  Slot 1 has no IOMUX pins on the P4, so all six lines must be
 * matrix-routed; 51k on the data lines is why the CMD53 data phase is worth
 * sweeping down to 400 kHz (`wifi slow`) before blaming the wire.
 * ====================================================================== */
#define C6_SDIO_CLK         18
#define C6_SDIO_CMD         19
#define C6_SDIO_D0          14
#define C6_SDIO_D1          15
#define C6_SDIO_D2          16
#define C6_SDIO_D3          17
#define C6_RESET            54  /* -> C6 CHIP_PU (R54 0R), active low */
#define C6_IO2_STRAP        6   /* -> C6 GPIO2 (R52 0R, download-mode strap) */

/* ======================================================================
 * USB / system
 * ====================================================================== */
#define USB_DM              45
#define USB_DP              46
#define BUTTON_BOOT         0
#define PWR_KEY             49  /* AXP2101 PWRON (power button) */

#endif                          /* _BOARD_H_ */
