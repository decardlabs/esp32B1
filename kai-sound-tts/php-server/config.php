<?php

return [
    'mqtt' => [
        'host'      => 'YOUR_MQTT_HOST',
        'port'      => 1883,
        'client_id' => 'php-server-' . uniqid(),
        'username'  => 'YOUR_MQTT_USERNAME',
        'password'  => 'YOUR_MQTT_PASSWORD',
    ],
    'websocket' => [
        'host' => '0.0.0.0',
        'port' => 8080,
    ],
    'topics' => [
        'device_status'    => 'device/+/status',
        'device_heartbeat' => 'device/+/heartbeat',
        'device_command'   => 'device/{device_id}/command',
    ],
    'device' => [
        'heartbeat_timeout' => 30,
    ],
    'internal_bridge' => [
        'port' => 8081,
    ],
    'python_tts' => [
        'prepare_url' => 'http://127.0.0.1:9100/api/tts',
        'timeout' => 20,
    ],
];
