/**
 * @file      utilities.h
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2023-04-11
 *
 */
#pragma once



//! The board peripheral power control pin needs to be set to HIGH when using the peripheral
#define BOARD_POWERON        GPIO_NUM_10

#define BOARD_I2S_WS         GPIO_NUM_5
#define BOARD_I2S_BCK        GPIO_NUM_7
#define BOARD_I2S_DOUT       GPIO_NUM_6

#define BOARD_I2C_SDA        GPIO_NUM_18
#define BOARD_I2C_SCL        GPIO_NUM_8

#define BOARD_BAT_ADC        GPIO_NUM_4

#define BOARD_TOUCH_INT      GPIO_NUM_16
#define BOARD_KEYBOARD_INT   GPIO_NUM_46

#define BOARD_SDCARD_CS      GPIO_NUM_39
#define BOARD_TFT_CS         GPIO_NUM_12
#define RADIO_CS_PIN         GPIO_NUM_9

#define BOARD_TFT_DC         GPIO_NUM_11
#define BOARD_TFT_BACKLIGHT  GPIO_NUM_42

#define BOARD_SPI_MOSI       GPIO_NUM_41
#define BOARD_SPI_MISO       GPIO_NUM_38
#define BOARD_SPI_SCK        GPIO_NUM_40

#define BOARD_TBOX_G02       GPIO_NUM_2
#define BOARD_TBOX_G01       GPIO_NUM_3
#define BOARD_TBOX_G04       GPIO_NUM_1
#define BOARD_TBOX_G03       GPIO_NUM_15

#define BOARD_ES7210_MCLK    GPIO_NUM_48
#define BOARD_ES7210_LRCK    GPIO_NUM_21
#define BOARD_ES7210_SCK     GPIO_NUM_47
#define BOARD_ES7210_DIN     GPIO_NUM_14

#define RADIO_BUSY_PIN       GPIO_NUM_13
#define RADIO_RST_PIN        GPIO_NUM_17
#define RADIO_DIO1_PIN       GPIO_NUM_45

#define BOARD_BOOT_PIN       GPIO_NUM_0

#define BOARD_BL_PIN         GPIO_NUM_42


#define BOARD_GPS_TX_PIN     GPIO_NUM_43
#define BOARD_GPS_RX_PIN     GPIO_NUM_44


#ifndef RADIO_FREQ
#define RADIO_FREQ           915.0
#endif

#ifndef RADIO_BANDWIDTH
#define RADIO_BANDWIDTH      125.0
#endif

#ifndef RADIO_SF
#define RADIO_SF             10
#endif

#ifndef RADIO_CR
#define RADIO_CR             6
#endif

#ifndef RADIO_TX_POWER
#define RADIO_TX_POWER       22
#endif

#define DEFAULT_OPA          100