#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include <stdbool.h>
#include "esp_err.h"

/* Callback when an MQTT command payload arrives */
typedef void (*mqtt_cmd_cb_t)(const char *payload, int len);

esp_err_t wifi_mqtt_init(mqtt_cmd_cb_t on_command);
bool wifi_mqtt_is_connected(void);
const char *wifi_mqtt_get_ip(void);
int wifi_mqtt_get_rssi(void);

/* Publish status/large payload */
esp_err_t wifi_mqtt_publish(const char *topic, const char *payload, bool retained);

#endif /* WIFI_MQTT_H */
