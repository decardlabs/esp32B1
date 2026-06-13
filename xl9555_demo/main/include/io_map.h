#ifndef IO_MAP_H
#define IO_MAP_H

#include <stdint.h>

/*
 * XL9555 logical IO map
 * Based on mapping extracted from the original project.
 */
typedef struct {
    uint8_t port;
    uint8_t pin;
} io_map_t;

/* Keys */
#define key1_port 0
#define key1_pin  4
#define key2_port 0
#define key2_pin  5
#define key3_port 0
#define key3_pin  6

/* LEDs */
#define led1_port 1
#define led1_pin  4
#define led2_port 1
#define led2_pin  5
#define led3_port 1
#define led3_pin  6

/* Buzzer */
#define beep_port 0
#define beep_pin  1

/* Optional tuple-style constants */
static const io_map_t key1 = {key1_port, key1_pin};
static const io_map_t key2 = {key2_port, key2_pin};
static const io_map_t key3 = {key3_port, key3_pin};

static const io_map_t led1 = {led1_port, led1_pin};
static const io_map_t led2 = {led2_port, led2_pin};
static const io_map_t led3 = {led3_port, led3_pin};

static const io_map_t beep = {beep_port, beep_pin};

#endif /* IO_MAP_H */
