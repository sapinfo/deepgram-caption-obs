# Test Connection Auto-Close

## Problem
`test_connection()` opened a WebSocket to verify API key validity but never closed it after success. This left an idle connection open, wasting Deepgram API resources and potentially causing issues if the user later started a real caption session.

## Solution
In the `Open` callback of `test_connection()`, immediately after confirming the connection succeeded:

1. Send `{"type":"CloseStream"}` — Deepgram's graceful disconnect message
2. Call `data->websocket->stop()` — close the WebSocket

```cpp
case ix::WebSocketMessageType::Open: {
    data->connected = true;
    update_text_display(data, "Connected OK!");
    obs_log(LOG_INFO, "Test connection: OK");

    // Close immediately after successful test
    std::string close_msg = "{\"type\":\"CloseStream\"}";
    data->websocket->sendText(close_msg);
    data->websocket->stop();
    break;
}
```

## Why CloseStream before stop()
Deepgram expects a `CloseStream` JSON message for graceful disconnect. Calling `stop()` alone would abruptly close the TCP connection. Sending `CloseStream` first follows the same pattern used in the normal caption stop flow.

## Date
2026-04-12
