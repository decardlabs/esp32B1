#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "driver/gpio.h"

/*
 * Default pins for quick bring-up:
 * - KEY_GPIO uses BOOT button on many dev boards.
 * - LED_GPIO uses onboard/user LED pin on some dev boards.
 * Change these to match your hardware.
 */
#define KEY_GPIO GPIO_NUM_0
#define LED_GPIO GPIO_NUM_2

/* Active levels */
#define KEY_ACTIVE_LEVEL 0
#define LED_ON_LEVEL 1

#endif /* BOARD_CONFIG_H */
