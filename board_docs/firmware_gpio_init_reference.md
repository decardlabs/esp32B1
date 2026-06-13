# Firmware GPIO Init Reference

Date: 2026-05-31
Board: LYIT_ESP32S3MB
Source: pin_mapping.json, pin_mapping_firmware.json

## 1) Camera (OV2640)

### Data bus
- D0: GPIO4
- D1: GPIO5
- D2: GPIO6
- D3: GPIO7
- D4: GPIO8
- D5: GPIO16
- D6: GPIO17
- D7: GPIO18

### Sync signals
- PCLK: GPIO45
- VSYNC: GPIO47
- HREF: GPIO48

### SCCB (camera I2C)
- OV_SCL: GPIO38
- OV_SDA: GPIO39

### Control via XL9555 (I2C expander)
- OV_RESET: XL9555 P0_3
- OV_PWDN: XL9555 P0_2

### XCLK
- Source: external 20MHz oscillator (not from ESP32-S3 GPIO)

## 2) Audio (ES8388 + MD8002A)

### I2S
- MCLK: GPIO3
- BCK/SCK: GPIO46
- LRCK: I2S_LRCK (GPIO not finalized in current mapping files)
- SDIN (to codec CDATA): I2S_SDIN (GPIO not finalized in current mapping files)
- SDOUT (from codec ASDOUT): I2S_SDOUT (GPIO not finalized in current mapping files)

### Codec control (main I2C)
- IIC_SCL: GPIO42
- IIC_SDA: GPIO41

### Amplifier enable
- SPK_EN: XL9555 P0_0

## 3) TFT / TF (shared SPI)

### Shared SPI bus
- MOSI: GPIO11 (with TFT_MOSI multiplexed)
- MISO: GPIO13
- SCK: GPIO12 (with TFT_CLK multiplexed)

### TF card
- TF_CS: GPIO40 (shared with IIC_INT/KEY0)

### TFT control
- TFT_DC: GPIO1
- TFT_RES: XL9555 P1_3
- TFT_BLK: XL9555 P1_1
- TFT_CS1/TFT_CS2: net confirmed, dedicated GPIO not listed in finalized JSON

## 4) Main I2C and Expander

### Main I2C bus
- SCL: GPIO42
- SDA: GPIO41
- Devices: XL9555, ES8388

### XL9555 interrupt
- INT: GPIO40

### XL9555 P0 map
- P0_0: SPK_EN
- P0_1: BEEP
- P0_2: OV_PWDN
- P0_3: OV_RESET
- P0_4: KEY3
- P0_5: KEY4
- P0_6: KEY3
- P0_7: KEY4

### XL9555 P1 map
- P1_0: NC
- P1_1: TFT_BLK
- P1_2: NC
- P1_3: TFT_RES
- P1_4: LED1
- P1_5: LED2
- P1_6: LED3
- P1_7: LED4

## 5) Other essential interfaces

- BOOT/LED0: GPIO0
- UART0 TX: U0_TXD (to CH340C RXD)
- UART0 RX: U0_RXD (from CH340C TXD)
- Native USB: USB_D+, USB_D-

## 6) Bring-up checklist (recommended order)

1. Initialize main I2C (SCL=GPIO42, SDA=GPIO41), verify XL9555 at 0x20.
2. Configure XL9555 outputs to safe defaults:
   - OV_PWDN asserted
   - OV_RESET asserted
   - SPK_EN disabled
   - TFT_BLK off
3. Initialize SPI (GPIO12/13/14) and TF/TFT chip select controls.
4. Initialize SCCB for camera (GPIO38/39), then release OV_RESET/OV_PWDN in sequence.
5. Initialize I2S with MCLK on GPIO3 and BCK on GPIO46.
6. Enable backlight/audio only after panel/codec init is stable.

## 7) Notes

- GPIO3 is confirmed as I2S_MCLK.
- OV2640 XCLK is external, so do not generate camera XCLK from ESP32-S3.
- I2S_LRCK=GPIO9 and I2S_SDIN=GPIO10 no longer overlap camera data pins after OV_D5/OV_D6 moved to GPIO16/GPIO17.
- SPI_SCK and TFT_CLK are multiplexed on GPIO12.
- SPI_MOSI and TFT_MOSI are multiplexed on GPIO11.
- GPIO40 is shared by IIC_INT, TF_CS, and KEY0.
- TFT_CS1/TFT_CS2 net confirmed, but dedicated GPIO numbers are not explicitly listed in JSON.