#include <stdbool.h>
#ifndef APP_WIFI_H
#define APP_WIFI_H

#include "esp_err.h"

/**
 * @brief Callback for Wi-Fi connection state changes.
 *
 * @param connected  true if Wi-Fi is connected and has an IP, false otherwise.
 * @param ip         The assigned IP string (empty string "" when disconnected).
 */
typedef void (*app_wifi_cb_t)(bool connected, const char *ip);

/**
 * @brief Start Wi-Fi station and connect to the given AP.
 *
 * This function initialises the ESP-NETIF, default event loop, Wi-Fi
 * driver, registers event handlers, and begins the connection process.
 * It is safe to call only once; subsequent calls will return ESP_FAIL
 * unless app_wifi_stop() is called first (not yet implemented).
 *
 * @param ssid     The SSID of the access point (must not be NULL).
 * @param password The WPA2-PSK passphrase (may be NULL or empty for
 *                 open networks; not guaranteed to work with all APs).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if ssid is NULL,
 *         ESP_FAIL if already initialised or Wi-Fi init fails.
 */
esp_err_t app_wifi_start(const char *ssid, const char *password);

/**
 * @brief Register a callback for connection state changes.
 *
 * Only one callback is supported; a second call overwrites the first.
 *
 * @param cb  Pointer to the callback function, or NULL to unregister.
 */
void app_wifi_register_cb(app_wifi_cb_t cb);

/**
 * @brief Get the current IP address string.
 *
 * @return Pointer to a static buffer containing the IP, or "0.0.0.0"
 *         if not yet connected.
 */
const char *app_wifi_get_ip(void);

/**
 * @brief Check whether the station is currently connected.
 *
 * @return true if connected and has an IP, false otherwise.
 */
bool app_wifi_is_connected(void);

#endif /* APP_WIFI_H */
