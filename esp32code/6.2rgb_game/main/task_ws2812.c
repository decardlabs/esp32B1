#include "task_ws2812.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "TASK_WS2812_GAME";

#define GAME_LED_COUNT             LED_STRIP_LED_COUNT
#define GAME_SHOOTER_ZONE_COUNT    30
#define GAME_LOOP_MS               10
#define GAME_BULLET_STEP_MS        70
#define GAME_ENEMY_STEP_MS         180
#define GAME_ENEMY_SPAWN_MS        650
#define GAME_FIRE_COOLDOWN_MS      120
#define GAME_HIT_BEEP_MS           80
#define GAME_MISS_BEEP_MS          160
#define GAME_BEEP_ON_LEVEL         0
#define GAME_BEEP_OFF_LEVEL        1

typedef enum {
    GAME_COLOR_NONE = 0,
    GAME_COLOR_RED,
    GAME_COLOR_GREEN,
    GAME_COLOR_BLUE,
} game_color_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} game_rgb_t;

typedef struct {
    game_color_t bullets[GAME_LED_COUNT];
    game_color_t enemies[GAME_LED_COUNT];
    bool key_pressed[3];
    uint32_t last_fire_ms[3];
    uint32_t last_bullet_ms;
    uint32_t last_enemy_ms;
    uint32_t last_spawn_ms;
    uint32_t beep_until_ms;
    uint32_t rng_state;
    uint32_t score;
    uint32_t miss;
} game_state_t;

static const game_rgb_t s_palette[] = {
    [GAME_COLOR_NONE]  = {0, 0, 0},
    [GAME_COLOR_RED]   = {70, 0, 0},
    [GAME_COLOR_GREEN] = {0, 70, 0},
    [GAME_COLOR_BLUE]  = {0, 0, 80},
};

static const uint8_t s_key_pins[] = {
    KEY1_PIN,
    KEY2_PIN,
    KEY3_PIN,
};

static const game_color_t s_key_colors[] = {
    GAME_COLOR_RED,
    GAME_COLOR_GREEN,
    GAME_COLOR_BLUE,
};

static uint32_t game_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool game_time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return (int32_t)(now_ms - target_ms) >= 0;
}

static bool game_period_elapsed(uint32_t now_ms, uint32_t last_ms, uint32_t period_ms)
{
    return (uint32_t)(now_ms - last_ms) >= period_ms;
}

static uint32_t game_random(game_state_t *game)
{
    uint32_t x = game->rng_state;

    if (x == 0) {
        x = 0x6d2b79f5u;
    }

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->rng_state = x;

    return x;
}

static game_color_t game_random_color(game_state_t *game)
{
    return (game_color_t)((game_random(game) % 3u) + GAME_COLOR_RED);
}

static void game_buzzer_set(bool on)
{
    xl9555_set_pin_level(BEEP_PORT, BEEP_PIN, on ? GAME_BEEP_ON_LEVEL : GAME_BEEP_OFF_LEVEL);
}

static void game_beep(game_state_t *game, uint32_t now_ms, uint32_t duration_ms)
{
    game->beep_until_ms = now_ms + duration_ms;
    game_buzzer_set(true);
}

static void game_service_buzzer(game_state_t *game, uint32_t now_ms)
{
    if (game->beep_until_ms != 0 && game_time_reached(now_ms, game->beep_until_ms)) {
        game->beep_until_ms = 0;
        game_buzzer_set(false);
    }
}

static bool game_hit_enemy(game_state_t *game, game_color_t bullet, game_color_t enemy, uint32_t now_ms)
{
    if (bullet == enemy) {
        game->score++;
        return true;
    }

    game->miss++;
    game_beep(game, now_ms, GAME_HIT_BEEP_MS);
    return false;
}

static void game_resolve_overlaps(game_state_t *game, uint32_t now_ms)
{
    for (int i = 0; i < GAME_LED_COUNT; i++) {
        if (game->bullets[i] == GAME_COLOR_NONE || game->enemies[i] == GAME_COLOR_NONE) {
            continue;
        }

        if (game_hit_enemy(game, game->bullets[i], game->enemies[i], now_ms)) {
            game->enemies[i] = GAME_COLOR_NONE;
        }
        game->bullets[i] = GAME_COLOR_NONE;
    }
}

static void game_fire(game_state_t *game, game_color_t color, uint32_t now_ms)
{
    if (game->bullets[0] == GAME_COLOR_NONE && game->enemies[0] == GAME_COLOR_NONE) {
        game->bullets[0] = color;
        return;
    }

    game_beep(game, now_ms, GAME_HIT_BEEP_MS);
}

static void game_poll_keys(game_state_t *game, uint32_t now_ms)
{
    for (int i = 0; i < 3; i++) {
        bool level = true;

        xl9555_get_pin_level(KEY_PORT, s_key_pins[i], &level);
        bool pressed = (level == 0);

        if (pressed && !game->key_pressed[i] &&
            game_period_elapsed(now_ms, game->last_fire_ms[i], GAME_FIRE_COOLDOWN_MS)) {
            game_fire(game, s_key_colors[i], now_ms);
            game->last_fire_ms[i] = now_ms;
        }

        game->key_pressed[i] = pressed;
    }
}

static void game_spawn_enemy(game_state_t *game)
{
    int start = GAME_LED_COUNT - 1;

    if (game->enemies[start] == GAME_COLOR_NONE && game->bullets[start] == GAME_COLOR_NONE) {
        game->enemies[start] = game_random_color(game);
    }
}

static void game_move_bullets(game_state_t *game, uint32_t now_ms)
{
    for (int i = GAME_LED_COUNT - 1; i >= 0; i--) {
        game_color_t color = game->bullets[i];

        if (color == GAME_COLOR_NONE) {
            continue;
        }

        game->bullets[i] = GAME_COLOR_NONE;
        int next = i + 1;

        if (next >= GAME_LED_COUNT) {
            continue;
        }

        if (game->enemies[next] != GAME_COLOR_NONE) {
            if (game_hit_enemy(game, color, game->enemies[next], now_ms)) {
                game->enemies[next] = GAME_COLOR_NONE;
            }
            continue;
        }

        if (game->bullets[next] == GAME_COLOR_NONE) {
            game->bullets[next] = color;
        }
    }
}

static void game_move_enemies(game_state_t *game, uint32_t now_ms)
{
    for (int i = 0; i < GAME_LED_COUNT; i++) {
        game_color_t color = game->enemies[i];

        if (color == GAME_COLOR_NONE) {
            continue;
        }

        game->enemies[i] = GAME_COLOR_NONE;
        int next = i - 1;

        if (next < 0) {
            game->miss++;
            game_beep(game, now_ms, GAME_MISS_BEEP_MS);
            continue;
        }

        if (game->bullets[next] != GAME_COLOR_NONE) {
            if (game_hit_enemy(game, game->bullets[next], color, now_ms)) {
                game->bullets[next] = GAME_COLOR_NONE;
            } else {
                game->bullets[next] = GAME_COLOR_NONE;
                game->enemies[next] = color;
            }
            continue;
        }

        if (game->enemies[next] == GAME_COLOR_NONE) {
            game->enemies[next] = color;
        } else {
            game->enemies[i] = color;
        }
    }
}

static void game_render(led_strip_handle_t strip, const game_state_t *game)
{
    for (int i = 0; i < GAME_LED_COUNT; i++) {
        game_color_t color = game->enemies[i];

        if (game->bullets[i] != GAME_COLOR_NONE) {
            color = game->bullets[i];
        }

        const game_rgb_t *rgb = &s_palette[color];
        ESP_ERROR_CHECK(led_strip_set_pixel(strip, i, rgb->r, rgb->g, rgb->b));
    }

    if (game->bullets[GAME_SHOOTER_ZONE_COUNT - 1] == GAME_COLOR_NONE &&
        game->enemies[GAME_SHOOTER_ZONE_COUNT - 1] == GAME_COLOR_NONE) {
        ESP_ERROR_CHECK(led_strip_set_pixel(strip, GAME_SHOOTER_ZONE_COUNT - 1, 3, 3, 3));
    }

    if (game->bullets[GAME_SHOOTER_ZONE_COUNT] == GAME_COLOR_NONE &&
        game->enemies[GAME_SHOOTER_ZONE_COUNT] == GAME_COLOR_NONE) {
        ESP_ERROR_CHECK(led_strip_set_pixel(strip, GAME_SHOOTER_ZONE_COUNT, 3, 3, 3));
    }

    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static void game_io_init(void)
{
    xl9555_init();

    xl9555_set_pin_dir(KEY_PORT, KEY1_PIN, 1);
    xl9555_set_pin_dir(KEY_PORT, KEY2_PIN, 1);
    xl9555_set_pin_dir(KEY_PORT, KEY3_PIN, 1);

    xl9555_set_pin_dir(BEEP_PORT, BEEP_PIN, 0);
    game_buzzer_set(false);

    xl9555_set_pin_dir(LED_PORT, LED1_PIN, 0);
    xl9555_set_pin_dir(LED_PORT, LED2_PIN, 0);
    xl9555_set_pin_dir(LED_PORT, LED3_PIN, 0);
    xl9555_set_pin_dir(LED_PORT, LED4_PIN, 0);
    xl9555_set_pin_level(LED_PORT, LED1_PIN, 1);
    xl9555_set_pin_level(LED_PORT, LED2_PIN, 1);
    xl9555_set_pin_level(LED_PORT, LED3_PIN, 1);
    xl9555_set_pin_level(LED_PORT, LED4_PIN, 1);

    ws2812_txs0108_enable();
}

void ws2812_task(void *pvParameters)
{
    (void)pvParameters;

    game_state_t game = {0};
    uint32_t now_ms = game_now_ms();

    game.rng_state = 0xa341316cu ^ now_ms;
    game.last_bullet_ms = now_ms;
    game.last_enemy_ms = now_ms;
    game.last_spawn_ms = now_ms - GAME_ENEMY_SPAWN_MS;

    game_io_init();

    led_strip_handle_t strip = ws2812_init();
    ESP_ERROR_CHECK(led_strip_clear(strip));
    ESP_ERROR_CHECK(led_strip_refresh(strip));

    ESP_LOGI(TAG, "RGB match game started. KEY1=red, KEY2=green, KEY3=blue.");

    while (1) {
        now_ms = game_now_ms();

        game_poll_keys(&game, now_ms);
        game_resolve_overlaps(&game, now_ms);

        if (game_period_elapsed(now_ms, game.last_spawn_ms, GAME_ENEMY_SPAWN_MS)) {
            game_spawn_enemy(&game);
            game.last_spawn_ms = now_ms;
        }

        if (game_period_elapsed(now_ms, game.last_bullet_ms, GAME_BULLET_STEP_MS)) {
            game_move_bullets(&game, now_ms);
            game.last_bullet_ms = now_ms;
        }

        if (game_period_elapsed(now_ms, game.last_enemy_ms, GAME_ENEMY_STEP_MS)) {
            game_move_enemies(&game, now_ms);
            game.last_enemy_ms = now_ms;
        }

        game_service_buzzer(&game, now_ms);
        game_render(strip, &game);

        vTaskDelay(pdMS_TO_TICKS(GAME_LOOP_MS));
    }
}

BaseType_t ws2812_task_create(void)
{
    return xTaskCreate(ws2812_task, "ws2812_task", 4096, NULL, 10, NULL);
}
