<?php

header('Content-Type: application/json; charset=utf-8');

if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
    http_response_code(405);
    echo json_encode(['ok' => false, 'error' => 'Method not allowed'], JSON_UNESCAPED_UNICODE);
    exit;
}

function fail(int $status, string $message, array $extra = []): void
{
    http_response_code($status);
    echo json_encode(array_merge(['ok' => false, 'error' => $message], $extra), JSON_UNESCAPED_UNICODE);
    exit;
}

$requestId = trim((string)($_GET['request_id'] ?? ''));
if ($requestId === '') {
    fail(400, 'Missing request_id');
}

$config = require __DIR__ . '/../config.php';
$ttsConfig = $config['python_tts'];
$basePrepareUrl = rtrim((string)$ttsConfig['prepare_url'], '/');
$statusUrl = preg_replace('#/api/tts$#', '/api/tts/' . rawurlencode($requestId), $basePrepareUrl);

if (!$statusUrl || $statusUrl === $basePrepareUrl) {
    fail(500, 'Unable to build status URL');
}

$context = stream_context_create([
    'http' => [
        'method' => 'GET',
        'timeout' => (int)($ttsConfig['timeout'] ?? 20),
        'ignore_errors' => true,
    ],
]);

$responseBody = @file_get_contents($statusUrl, false, $context);
$headers = $http_response_header ?? [];
$statusCode = 0;
if (!empty($headers) && preg_match('#HTTP/\S+\s+(\d{3})#', $headers[0], $matches)) {
    $statusCode = (int)$matches[1];
}

if ($responseBody === false) {
    $error = error_get_last();
    fail(502, 'Python TTS status request failed', ['detail' => $error['message'] ?? 'request failed']);
}

$decoded = json_decode($responseBody, true);
if (!is_array($decoded)) {
    fail(502, 'Python TTS status returned invalid json');
}

http_response_code($statusCode > 0 ? $statusCode : 200);
echo json_encode($decoded, JSON_UNESCAPED_UNICODE);
