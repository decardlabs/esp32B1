#ifndef XL9555_H
#define XL9555_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t xl9555_init(void);
esp_err_t xl9555_speaker_enable(bool on);

#endif /* XL9555_H */
