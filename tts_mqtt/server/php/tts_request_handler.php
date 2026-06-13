<?php

use PhpMqtt\Client\ConnectionSettings;
use PhpMqtt\Client\MqttClient;

function tts_fail(int $status, string $message, array $extra = []): void
{
    http_response_code($status);
    echo json_encode(array_merge(['ok' => false, 'error' => $message], $extra), JSON_UNESCAPED_UNICODE);
    exit;
}

function tts_post_json(string $url, array $payload, int $timeout): array
{
    $body = json_encode($payload, JSON_UNESCAPED_UNICODE);
    $context = stream_context_create([
        'http' => [
            'method'        => 'POST',
            'header'        => "Content-Type: application/json\r\nAccept: application/json\r\n",
            'content'       => $body,
            'timeout'       => $timeout,
            'ignore_errors' => true,
        ],
    ]);

    $responseBody = @file_get_contents($url, false, $context);
    $headers = $http_response_header ?? [];
    $statusCode = 0;
    if (!empty($headers) && preg_match('#HTTP/\S+\s+(\d{3})#', $headers[0], $matches)) {
        $statusCode = (int) $matches[1];
    }

    if ($responseBody === false) {
        $error = error_get_last();
        throw new RuntimeException($error['message'] ?? 'request failed');
    }

    $decoded = json_decode($responseBody, true);
    if (!is_array($decoded)) {
        throw new RuntimeException('python bridge returned invalid json');
    }

    return [
        'status_code' => $statusCode,
        'body' => $decoded,
    ];
}

function tts_make_preview(string $text, int $limit = 60): string
{
    $text = preg_replace('/\s+/u', ' ', trim($text)) ?? trim($text);
    if (mb_strlen($text, 'UTF-8') <= $limit) {
        return $text;
    }

    return rtrim(mb_substr($text, 0, $limit, 'UTF-8')) . '...';
}

function tts_chunk_by_length(string $text, int $maxChars): array
{
    $chunks = [];
    $length = mb_strlen($text, 'UTF-8');
    for ($offset = 0; $offset < $length; $offset += $maxChars) {
        $part = trim(mb_substr($text, $offset, $maxChars, 'UTF-8'));
        if ($part !== '') {
            $chunks[] = $part;
        }
    }
    return $chunks;
}

function tts_split_overlong_sentence(string $text, int $hardMaxChars): array
{
    $pieces = preg_split('/(?<=[，,、：:])/u', $text, -1, PREG_SPLIT_NO_EMPTY) ?: [$text];
    $result = [];

    foreach ($pieces as $piece) {
        $piece = trim($piece);
        if ($piece === '') {
            continue;
        }

        if (mb_strlen($piece, 'UTF-8') > $hardMaxChars) {
            $result = array_merge($result, tts_chunk_by_length($piece, $hardMaxChars));
            continue;
        }

        $result[] = $piece;
    }

    return $result;
}

function tts_split_text_into_segments(string $text, int $targetChars = 70, int $hardMaxChars = 100): array
{
    $text = str_replace(["\r\n", "\r"], "\n", trim($text));
    if ($text === '') {
        return [];
    }

    $paragraphs = preg_split('/\n+/u', $text, -1, PREG_SPLIT_NO_EMPTY) ?: [$text];
    $sentences = [];

    foreach ($paragraphs as $paragraph) {
        $paragraph = trim($paragraph);
        if ($paragraph === '') {
            continue;
        }

        $parts = preg_split('/(?<=[。！？!?；;])/u', $paragraph, -1, PREG_SPLIT_NO_EMPTY) ?: [$paragraph];
        foreach ($parts as $part) {
            $part = trim($part);
            if ($part === '') {
                continue;
            }

            if (mb_strlen($part, 'UTF-8') > $hardMaxChars) {
                $sentences = array_merge($sentences, tts_split_overlong_sentence($part, $hardMaxChars));
                continue;
            }

            $sentences[] = $part;
        }
    }

    $segments = [];
    $current = '';

    foreach ($sentences as $sentence) {
        if ($current === '') {
            $current = $sentence;
            continue;
        }

        $candidate = $current . $sentence;
        if (mb_strlen($candidate, 'UTF-8') <= $targetChars) {
            $current = $candidate;
            continue;
        }

        $segments[] = $current;
        $current = $sentence;
    }

    if ($current !== '') {
        $segments[] = $current;
    }

    return array_values(array_filter(array_map(
        static fn (string $segment): string => trim($segment),
        $segments
    )));
}

function tts_build_bridge_payload(array $data, string $deviceId, string $text, string $voice, string $format, bool $playImmediately): array
{
    return [
        'device_id' => $deviceId,
        'text' => $text,
        'voice' => $voice,
        'format' => $format,
        'sample_rate' => (int) ($data['sample_rate'] ?? 16000),
        'volume' => (int) ($data['volume'] ?? 80),
        'speech_rate' => (int) ($data['speech_rate'] ?? 0),
        'pitch_rate' => (int) ($data['pitch_rate'] ?? 0),
        'play_immediately' => $playImmediately,
    ];
}

function tts_create_bridge_segment(array $ttsConfig, array $payload): array
{
    $bridgeResponse = tts_post_json($ttsConfig['prepare_url'], $payload, (int) $ttsConfig['timeout']);
    $bridgeBody = $bridgeResponse['body'];

    if (($bridgeResponse['status_code'] >= 400) || empty($bridgeBody['ok']) || empty($bridgeBody['stream_url'])) {
        throw new RuntimeException('Python TTS bridge returned error: ' . json_encode($bridgeBody, JSON_UNESCAPED_UNICODE));
    }

    return $bridgeBody;
}

function tts_build_mqtt_settings(array $mqtt): ConnectionSettings
{
    $settings = (new ConnectionSettings())->setKeepAliveInterval(10);

    if (!empty($mqtt['username'])) {
        $settings = $settings->setUsername($mqtt['username']);
        if (!empty($mqtt['password'])) {
            $settings = $settings->setPassword($mqtt['password']);
        }
    }

    return $settings;
}

function tts_publish_command(MqttClient $client, string $topic, array $commandPayload): void
{
    $client->publish($topic, json_encode($commandPayload, JSON_UNESCAPED_UNICODE), 1);
    $client->loop(true, true, 1);
}

function tts_wait_for_segment_finish(
    MqttClient $client,
    string $statusTopic,
    string $streamUrl,
    string $textPreview,
    int $timeoutSeconds
): array {
    $state = [
        'started' => false,
        'completed' => false,
        'timed_out' => false,
        'last_status' => null,
        'last_error' => '',
    ];

    $subscription = function (string $topic, string $message, bool $retained, array $matchedWildcards) use (&$state, $client, $streamUrl, $textPreview): void {
        $decoded = json_decode($message, true);
        if (!is_array($decoded)) {
            return;
        }

        $state['last_status'] = $decoded;
        $playState = (string) ($decoded['play_state'] ?? '');
        $currentUrl = (string) ($decoded['current_url'] ?? '');
        $currentText = trim((string) ($decoded['current_text'] ?? ''));
        $lastError = trim((string) ($decoded['last_error'] ?? ''));

        if ($currentUrl === $streamUrl || ($currentText !== '' && $currentText === $textPreview && in_array($playState, ['playing', 'paused'], true))) {
            $state['started'] = true;
        }

        if ($state['started'] && $playState === 'idle') {
            $state['completed'] = true;
            $state['last_error'] = $lastError;
            $client->interrupt();
        }
    };

    $timeoutWatcher = function (MqttClient $mqtt, float $elapsedTime) use (&$state, $timeoutSeconds): void {
        if ($elapsedTime >= $timeoutSeconds) {
            $state['timed_out'] = true;
            $mqtt->interrupt();
        }
    };

    $client->subscribe($statusTopic, $subscription, 1);
    $client->registerLoopEventHandler($timeoutWatcher);
    $client->loop(true);
    $client->unsubscribe($statusTopic);
    $client->unregisterLoopEventHandler($timeoutWatcher);

    return $state;
}

function tts_handle_request(string $configPath): void
{
    header('Content-Type: application/json; charset=utf-8');
    set_time_limit(0);

    if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
        tts_fail(405, 'Method not allowed');
    }

    $raw = file_get_contents('php://input');
    $data = json_decode($raw, true);
    if (!is_array($data)) {
        tts_fail(400, 'Invalid JSON');
    }

    $deviceId = trim((string) ($data['device_id'] ?? 'esp32_01'));
    $text = trim((string) ($data['text'] ?? ''));
    $voice = trim((string) ($data['voice'] ?? 'ailun'));
    $format = trim((string) ($data['format'] ?? 'mp3'));
    $playImmediately = (bool) ($data['play_immediately'] ?? true);

    if ($deviceId === '') {
        tts_fail(400, 'Missing device_id');
    }
    if ($text === '') {
        tts_fail(400, 'Missing text');
    }

    $config = require $configPath;
    $ttsConfig = $config['python_tts'];
    $segments = tts_split_text_into_segments($text, 70, 100);
    if (empty($segments)) {
        tts_fail(400, 'Text split failed');
    }

    if (!$playImmediately || count($segments) === 1) {
        $ttsPayload = tts_build_bridge_payload($data, $deviceId, $text, $voice, $format, $playImmediately);

        try {
            $bridgeBody = tts_create_bridge_segment($ttsConfig, $ttsPayload);
        } catch (Throwable $e) {
            tts_fail(502, 'Python TTS bridge request failed', ['detail' => $e->getMessage()]);
        }

        $result = [
            'ok' => true,
            'tts' => $bridgeBody,
            'sequence' => [
                'enabled' => false,
                'segment_count' => 1,
                'segments' => [$text],
            ],
        ];

        if (!$playImmediately) {
            echo json_encode($result, JSON_UNESCAPED_UNICODE);
            exit;
        }

        $mqtt = $config['mqtt'];
        $topic = str_replace('{device_id}', $deviceId, $config['topics']['device_command']);
        $commandPayload = [
            'cmd' => 'tts',
            'url' => $bridgeBody['stream_url'],
            'request_id' => $bridgeBody['request_id'] ?? '',
            'text_preview' => $bridgeBody['text_preview'] ?? '',
            'voice' => $voice,
            'format' => $format,
        ];

        try {
            $client = new MqttClient($mqtt['host'], $mqtt['port'], 'php-tts-' . uniqid(), MqttClient::MQTT_3_1_1);
            $client->connect(tts_build_mqtt_settings($mqtt), true);
            tts_publish_command($client, $topic, $commandPayload);
            $client->disconnect();

            $result['topic'] = $topic;
            $result['command'] = $commandPayload;
            echo json_encode($result, JSON_UNESCAPED_UNICODE);
            exit;
        } catch (Throwable $e) {
            tts_fail(500, 'MQTT publish failed', [
                'detail' => $e->getMessage(),
                'tts' => $bridgeBody,
            ]);
        }
    }

    $mqtt = $config['mqtt'];
    $commandTopic = str_replace('{device_id}', $deviceId, $config['topics']['device_command']);
    $statusTopic = str_replace('+', $deviceId, $config['topics']['device_status']);
    $preparedSegments = [];
    $results = [];

    try {
        foreach ($segments as $index => $segmentText) {
            $segmentPayload = tts_build_bridge_payload($data, $deviceId, $segmentText, $voice, $format, true);
            $bridgeBody = tts_create_bridge_segment($ttsConfig, $segmentPayload);

            $preparedSegments[] = [
                'index' => $index,
                'text' => $segmentText,
                'bridge' => $bridgeBody,
                'command' => [
                    'cmd' => 'tts',
                    'url' => $bridgeBody['stream_url'],
                    'request_id' => $bridgeBody['request_id'] ?? '',
                    'text_preview' => $bridgeBody['text_preview'] ?? tts_make_preview($segmentText),
                    'voice' => $voice,
                    'format' => $format,
                ],
            ];
        }

        $client = new MqttClient($mqtt['host'], $mqtt['port'], 'php-tts-seq-' . uniqid(), MqttClient::MQTT_3_1_1);
        $client->connect(tts_build_mqtt_settings($mqtt), true);

        foreach ($preparedSegments as $prepared) {
            $index = $prepared['index'];
            $segmentText = $prepared['text'];
            $bridgeBody = $prepared['bridge'];
            $commandPayload = $prepared['command'];

            tts_publish_command($client, $commandTopic, $commandPayload);

            $waitState = tts_wait_for_segment_finish(
                $client,
                $statusTopic,
                $commandPayload['url'],
                $commandPayload['text_preview'],
                45
            );

            $segmentResult = [
                'index' => $index,
                'text' => $segmentText,
                'text_preview' => $commandPayload['text_preview'],
                'request_id' => $bridgeBody['request_id'] ?? '',
                'stream_url' => $bridgeBody['stream_url'],
                'status_url' => $bridgeBody['status_url'] ?? '',
                'completed' => $waitState['completed'],
                'timed_out' => $waitState['timed_out'],
                'last_error' => $waitState['last_error'],
            ];

            $results[] = $segmentResult;

            if ($waitState['timed_out']) {
                throw new RuntimeException('Segment timeout at index ' . $index);
            }

            if ($waitState['last_error'] !== '') {
                throw new RuntimeException('Device playback error at index ' . $index . ': ' . $waitState['last_error']);
            }
        }

        $client->disconnect();
    } catch (Throwable $e) {
        tts_fail(500, 'TTS sequence failed', [
            'detail' => $e->getMessage(),
            'sequence' => [
                'enabled' => true,
                'segment_count' => count($segments),
                'prepared_count' => count($preparedSegments),
                'completed_count' => count($results),
                'results' => $results,
            ],
        ]);
    }

    $first = $results[0];
    echo json_encode([
        'ok' => true,
        'tts' => [
            'ok' => true,
            'request_id' => $first['request_id'],
            'stream_url' => $first['stream_url'],
            'status_url' => $first['status_url'],
            'text_preview' => $first['text_preview'],
            'provider' => 'volcengine',
        ],
        'topic' => $commandTopic,
        'command' => [
            'cmd' => 'tts',
            'url' => $first['stream_url'],
            'request_id' => $first['request_id'],
            'text_preview' => $first['text_preview'],
            'voice' => $voice,
            'format' => $format,
        ],
        'sequence' => [
            'enabled' => true,
            'segment_count' => count($segments),
            'segments' => $segments,
            'prepared_count' => count($preparedSegments),
            'results' => $results,
        ],
    ], JSON_UNESCAPED_UNICODE);
    exit;
}
