#ifndef GPIO_DEFINE_H
#define GPIO_DEFINE_H

// Generated from finalized mapping files on 2026-05-31.
// Board: LYIT_ESP32S3MB

#ifdef __cplusplus
extern "C" {
#endif

// =========================
// ESP32-S3 GPIO definitions
// =========================

// Boot / basic
#define GPIO_BOOT_KEY_LED0        0

// TFT / touch / SPI shared bus
#define GPIO_TFT_DC               1
#define GPIO_TFT_PEN              2
#define GPIO_SPI_MOSI            11
#define GPIO_TFT_MOSI            11
#define GPIO_SPI_MISO            13
#define GPIO_SPI_SCK             12
#define GPIO_TFT_CLK             12
#define GPIO_TF_CS               40
#define GPIO_KEY0                40

// Camera data bus (OV2640)
#define GPIO_OV_D0                4
#define GPIO_OV_D1                5
#define GPIO_OV_D2                6
#define GPIO_OV_D3                7
#define GPIO_OV_D4                8
#define GPIO_OV_D5               16
#define GPIO_OV_D6               17
#define GPIO_OV_D7               18

// Camera SCCB
#define GPIO_OV_SCL              38
#define GPIO_OV_SDA              39

// Camera sync
#define GPIO_OV_PCLK             45
#define GPIO_I2S_BCK             46
#define GPIO_OV_VSYNC            47
#define GPIO_OV_HREF             48

// I2C main bus + expander interrupt
#define GPIO_IIC_INT             40
#define GPIO_IIC_SCL             42
#define GPIO_IIC_SDA             41

// I2S
#define GPIO_I2S_MCLK             3
#define GPIO_I2S_LRCK             9
#define GPIO_I2S_SDIN            10
#define GPIO_I2S_SDOUT           14

// Multiplexing note:
// GPIO12 is shared between SPI_SCK and TFT_CLK.
// GPIO11 is shared between SPI_MOSI and TFT_MOSI.
// GPIO40 is shared between IIC_INT, TF_CS, and KEY0.

// =========================
// XL9555 definitions
// =========================

#define XL9555_I2C_ADDR       0x20

// XL9555 P0 bit map
#define XL9555_P0_SPK_EN         0
#define XL9555_P0_BEEP           1
#define XL9555_P0_OV_PWDN        2
#define XL9555_P0_OV_RESET       3
#define XL9555_P0_KEY3_LEGACY    4
#define XL9555_P0_KEY4           5
#define XL9555_P0_KEY3           6
#define XL9555_P0_KEY4_ALT       7

// XL9555 P1 bit map
#define XL9555_P1_NC0            0
#define XL9555_P1_TFT_BLK        1
#define XL9555_P1_NC2            2
#define XL9555_P1_TFT_RES        3
#define XL9555_P1_LED1           4
#define XL9555_P1_LED2           5
#define XL9555_P1_LED3           6
#define XL9555_P1_LED4           7

// Logical aliases
#define XL9555_SPK_EN_BIT        XL9555_P0_SPK_EN
#define XL9555_OV_RESET_BIT      XL9555_P0_OV_RESET
#define XL9555_OV_PWDN_BIT       XL9555_P0_OV_PWDN

#ifdef __cplusplus
}
#endif

#endif // GPIO_DEFINE_H
