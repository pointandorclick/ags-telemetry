# Plan: Add Network Sending via AGSsock

## Overview

Add optional TCP-based telemetry sending using the AGSsock plugin. Events are sent over a persistent TCP connection using the same pipe-delimited format already used for file logging. File logging is retained as a fallback. A compile-time `#define TELEMETRY_NETWORK` flag makes the feature opt-in.

## TCP Protocol

Simple text-based protocol over TCP:

```
# Handshake (client → server):
TELEMETRY|1|<api_key>|<build_version>|<platform_tag>\n

# Server response:
OK|<session_id>\n
ERROR|<reason>\n

# Events (same format as log file):
2025-03-23 10:30:01|room_enter|room_id=1|phase=before_fadein\n

# Reconnect handshake:
RESUME|1|<api_key>|<session_id>\n

# Session end: client sends session_end event, then closes connection
```

## Changes to Telemetry.ash

Add inside `#ifdef TELEMETRY_NETWORK`:
- `import String Telemetry_ServerHost;` — Server IP/hostname
- `import int Telemetry_ServerPort;` — TCP port (default 9001)
- `import String Telemetry_ApiKey;` — Auth key
- `import int Telemetry_SendIntervalSeconds;` — Flush interval (default 5)
- `import bool Telemetry_NetworkConnected;` — Read-only status

## Changes to Telemetry.asc

### 1. Network state variables
- `Socket *_telemetry_socket` — AGSsock TCP socket
- `SockAddr *_telemetry_serverAddr` — Server address
- `int _telemetry_netState` — State machine (0=DISCONNECTED, 1=CONNECTING, 2=HANDSHAKING, 3=CONNECTED)
- `int _telemetry_lastSendRaw` — Last flush timestamp
- `int _telemetry_reconnectAfterRaw` — When to attempt reconnect
- `int _telemetry_reconnectBackoff` — Current backoff (5→10→20→40→60s cap)
- `String _telemetry_serverSessionId` — Session ID from server handshake
- `int _telemetry_handshakeWaitStart` — When handshake was sent (for timeout)

### 2. Circular event queue
```
#define TELEMETRY_QUEUE_SIZE 500
String _telemetry_queue[TELEMETRY_QUEUE_SIZE];
int _telemetry_queueHead = 0;
int _telemetry_queueTail = 0;
int _telemetry_queueCount = 0;
```

Functions:
- `_Telemetry_Enqueue(String line)` — Add to queue, drop oldest if full
- `_Telemetry_Dequeue()` → String — Remove from front
- `_Telemetry_QueueIsEmpty()` → bool

### 3. Refactored _Telemetry_WriteLine
Extract file writing to `_Telemetry_WriteToFile(String line)`. The existing `_Telemetry_WriteLine` calls both `_Telemetry_WriteToFile` and `_Telemetry_Enqueue` (if TELEMETRY_NETWORK defined).

### 4. Connection management functions
- `_Telemetry_Connect()` — Create socket, resolve address, async connect
- `_Telemetry_Disconnect()` — Close socket, set state to DISCONNECTED
- `_Telemetry_SendHandshake()` — Send TELEMETRY or RESUME line
- `_Telemetry_CheckHandshakeResponse()` — Non-blocking recv, parse OK|session_id
- `_Telemetry_FlushQueue()` — Send up to 50 queued events per call
- `_Telemetry_ScheduleReconnect()` — Set reconnect timer with backoff

### 5. State machine in _Telemetry_NetworkTick(int nowRaw)

```
DISCONNECTED:
  if nowRaw >= _telemetry_reconnectAfterRaw:
    _Telemetry_Connect()
    state → CONNECTING

CONNECTING:
  if socket.Valid:
    _Telemetry_SendHandshake()
    state → HANDSHAKING
  elif socket error:
    _Telemetry_ScheduleReconnect()
    state → DISCONNECTED

HANDSHAKING:
  response = socket.Recv()
  if response starts with "OK|":
    extract session_id
    reset backoff
    state → CONNECTED
  elif response starts with "ERROR" or timeout (10s):
    _Telemetry_Disconnect()
    _Telemetry_ScheduleReconnect()
    state → DISCONNECTED

CONNECTED:
  if !socket.Valid:
    _Telemetry_Disconnect()
    _Telemetry_ScheduleReconnect()
    state → DISCONNECTED
  elif nowRaw - _telemetry_lastSendRaw >= Telemetry_SendIntervalSeconds:
    _Telemetry_FlushQueue()
    _telemetry_lastSendRaw = nowRaw
```

### 6. Modified Telemetry_StartSession()
After existing file writes, if TELEMETRY_NETWORK:
- Set `_telemetry_reconnectBackoff = 5`
- Call `_Telemetry_Connect()`

### 7. Modified Telemetry_EndSession()
If TELEMETRY_NETWORK and connected:
- Call `_Telemetry_FlushQueue()` (send remaining events)
- Call `_Telemetry_Disconnect()`

### 8. Modified TelemetryConfig_Init()
Add defaults inside `#ifdef TELEMETRY_NETWORK`:
```
Telemetry_ServerHost = "";
Telemetry_ServerPort = 9001;
Telemetry_ApiKey = "";
Telemetry_SendIntervalSeconds = 5;
```

## Not covered (separate projects)
- Dashboard server TCP listener implementation
- Screenshot/file uploads over network
- TLS encryption (AGSsock limitation)
