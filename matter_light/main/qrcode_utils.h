#ifndef QRCODE_UTILS_H
#define QRCODE_UTILS_H

#include "lvgl.h"

/**
 * @brief Generate an LVGL image descriptor from a text string using QR code
 *
 * @param text  Text to encode (e.g., Matter setup payload)
 * @param scale Pixel scale factor (1 = 1px per module, 4 = 4px per module)
 * @return lv_img_dsc_t*  Dynamically allocated image descriptor, or NULL on failure.
 *                         Caller must free the descriptor and the data when done.
 */
lv_img_dsc_t *qrcode_get_lvgl_image(const char *text, uint8_t scale);

#endif
