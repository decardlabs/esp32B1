# Touch Calibration Baseline (ESP32-S3 + ST7796 + XPT2046)

This file records the verified stable touch configuration for this project.
Use it as the source of truth when modifying touch code.

## Verified Stable Configuration

### Driver: `components/device/xpt2046.c`

- SPI mode: fixed `0`
- `XPT2046_SWAP_XY`: `0`
- `XPT2046_MIRROR_X`: `0`
- `XPT2046_MIRROR_Y`: `0`
- `XPT2046_RAW_X_MIN/MAX`: `220 / 3850`
- `XPT2046_RAW_Y_MIN/MAX`: `260 / 3820`

### LVGL UI: `main/task_lcd_lvgl.c`

- `LCD_BTN_HIT_INSET_PX`: `14`

## Maintenance Rules

1. Do not add a second custom coordinate mapping layer in `task_lcd_lvgl.c`.
2. If touch offset appears, tune only `RAW_X/Y_MIN/MAX` first.
3. If `Z` changes but `X/Y` are fixed, confirm SPI mode is still `0`.
4. If edge false-trigger appears, adjust `LCD_BTN_HIT_INSET_PX` in small steps (e.g., 14 -> 16 -> 18).

## Related Pin Map

- External reference file: `/Users/macairm5/Documents/esp32/PIN_MAP.md`
