<?php

return [
    'mqtt' => [
        'host' => '127.0.0.1',
        'port' => 1883,
        'client_id' => 'php-server-' . uniqid(),
        'username' => 'YOUR_MQTT_USER',
        'password' => 'YOUR_MQTT_PASSWORD',
    ],
    'python_tts' => [
        'prepare_url' => 'http://127.0.0.1:9100/api/tts',
        'timeout' => 30,
    ],
    'topics' => [
        'device_command' => 'device/{device_id}/command',
        'device_status'  => 'device/+/status',
    ],
];
