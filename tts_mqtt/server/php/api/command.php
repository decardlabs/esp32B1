<?php

require __DIR__ . '/../vendor/autoload.php';

use PhpMqtt\Client\ConnectionSettings;
use PhpMqtt\Client\MqttClient;

header('Content-Type: application/json; charset=utf-8');

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['ok' => false, 'error' => 'Method not allowed'], JSON_UNESCAPED_UNICODE);
    exit;
}

$raw = file_get_contents('php://input');
$data = json_decode($raw, true);
if (!is_array($data)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'Invalid JSON'], JSON_UNESCAPED_UNICODE);
    exit;
}

$deviceId = trim((string)($data['device_id'] ?? 'esp32_01'));
$cmd = trim((string)($data['cmd'] ?? ''));
$params = $data['params'] ?? [];

if ($deviceId === '' || $cmd === '') {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'Missing device_id or cmd'], JSON_UNESCAPED_UNICODE);
    exit;
}

if (!is_array($params)) {
    $params = [];
}

$config = require __DIR__ . '/../config.php';
$mqtt = $config['mqtt'];
$topic = str_replace('{device_id}', $deviceId, $config['topics']['device_command']);
$payload = json_encode(array_merge(['cmd' => $cmd], $params), JSON_UNESCAPED_UNICODE);

try {
    $client = new MqttClient($mqtt['host'], $mqtt['port'], 'php-http-' . uniqid(), MqttClient::MQTT_3_1_1);
    $settings = (new ConnectionSettings())->setKeepAliveInterval(10);

    if (!empty($mqtt['username'])) {
        $settings = $settings->setUsername($mqtt['username']);
        if (!empty($mqtt['password'])) {
            $settings = $settings->setPassword($mqtt['password']);
        }
    }

    $client->connect($settings, true);
    $client->publish($topic, $payload, 1);
    $client->disconnect();

    echo json_encode([
        'ok' => true,
        'topic' => $topic,
        'payload' => json_decode($payload, true),
    ], JSON_UNESCAPED_UNICODE);
} catch (Throwable $e) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => $e->getMessage()], JSON_UNESCAPED_UNICODE);
}
