--TEST--
Distributed tracing headers propagate with curl_exec()
--SKIPIF--
<?php if (!extension_loaded('curl')) die('skip: curl extension required'); ?>
<?php if (!getenv('HTTPBIN_HOSTNAME')) die('skip: HTTPBIN_HOSTNAME env var required'); ?>
--ENV--
DD_TRACE_DEBUG=1
--FILE--
<?php
dd_trace_distributed_tracing_headers([
    'foo-header: one',
    'x-header: two',
    'x-datadog-trace-id: 1234',
    'x-datadog-parent-id: 1337', // Should be replaced by active span ID
    'x-datadog-sampling-priority: 0.5',
]);

$port = getenv('HTTPBIN_PORT') ?: '80';
$url = 'http://' . getenv('HTTPBIN_HOSTNAME') . ':' . $port .'/headers';
$ch = curl_init();
curl_setopt($ch, CURLOPT_URL, $url);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
$response = curl_exec($ch);
curl_close($ch);

include 'distributed_tracing.inc';
dt_dump_headers_from_httpbin($response, [
    'foo-header',
    'x-header',
    'x-datadog-trace-id',
    'x-datadog-parent-id',
    'x-datadog-sampling-priority',
]);

echo 'Done.' . PHP_EOL;
?>
--EXPECT--
foo-header: one
x-datadog-sampling-priority: 0.5
x-datadog-trace-id: 1234
x-header: two
Done.
