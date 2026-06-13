<?php

require __DIR__ . "/../vendor/autoload.php";

use PhpMqtt\Client\ConnectionSettings;
use PhpMqtt\Client\MqttClient;

header("Content-Type: application/json; charset=utf-8");

if ($_SERVER["REQUEST_METHOD"] !== "GET") {
    http_response_code(405);
    echo json_encode(["ok" => false, "error" => "Method not allowed"], JSON_UNESCAPED_UNICODE);
    exit;
}

$deviceId = trim((string)($_GET["device_id"] ?? "esp32_01"));
if ($deviceId === "") {
    http_response_code(400);
    echo json_encode(["ok" => false, "error" => "Missing device_id"], JSON_UNESCAPED_UNICODE);
    exit;
}

$config = require __DIR__ . "/../config.php";
$mqtt = $config["mqtt"];
$topic = "device/" . $deviceId . "/status";

try {
    $client = new MqttClient($mqtt["host"], $mqtt["port"], "php-status-" . uniqid(), MqttClient::MQTT_3_1_1);
    $settings = (new ConnectionSettings())
        ->setKeepAliveInterval(10);

    if (!empty($mqtt["username"])) {
        $settings = $settings->setUsername($mqtt["username"]);
        if (!empty($mqtt["password"])) {
            $settings = $settings->setPassword($mqtt["password"]);
        }
    }

    $client->connect($settings, true);

    $status = null;
    $client->subscribe($topic, function (string $topic, string $msg, bool $retained, array $matchedWildcards) use ($client, &$status) {
        $decoded = json_decode($msg, true);
        if (is_array($decoded)) {
            $status = $decoded;
        }
        $client->interrupt();
    }, 1);

    $client->loop(true, true, 5);
    $client->disconnect();

    if ($status !== null) {
        $online = !empty($status["online"]);
        echo json_encode([
            "ok" => true,
            "device_id" => $deviceId,
            "online" => $online,
            "status" => $status,
        ], JSON_UNESCAPED_UNICODE);
    } else {
        echo json_encode([
            "ok" => true,
            "device_id" => $deviceId,
            "online" => false,
            "status" => null,
            "error" => "No status message received (device may be offline)",
        ], JSON_UNESCAPED_UNICODE);
    }
} catch (Throwable $e) {
    echo json_encode([
        "ok" => false,
        "online" => false,
        "error" => $e->getMessage(),
    ], JSON_UNESCAPED_UNICODE);
}
