# Telemetry TCP Server Implementation Guide

This document describes what a server needs to implement to accept telemetry data from AGS games using the `TELEMETRY_NETWORK` feature of the ags-telemetry module.

The game connects over raw TCP and sends events in a simple text-based protocol. The server's job is to accept connections, authenticate them, assign session IDs, receive pipe-delimited event lines, and feed them into your data pipeline (e.g., the [ags-telemetry-dashboard](https://github.com/pointandorclick/ags-telemetry-dashboard) REST API).

## Overview

```
┌──────────────┐         TCP (port 9001)         ┌──────────────────┐
│   AGS Game   │ ──────────────────────────────── │  TCP Listener    │
│  (AGSsock)   │   pipe-delimited text lines      │  (your server)   │
└──────────────┘                                  └────────┬─────────┘
                                                           │
                                                           │ parses lines,
                                                           │ calls REST API
                                                           ▼
                                                  ┌──────────────────┐
                                                  │    Dashboard     │
                                                  │    REST API      │
                                                  └──────────────────┘
```

The TCP listener acts as a bridge: it speaks the simple text protocol to the game client and translates into REST API calls to the dashboard.

## Protocol Specification

### Connection

- **Transport**: TCP
- **Default port**: 9001 (configurable on the game side via `Telemetry_ServerPort`)
- **Encoding**: UTF-8 text
- **Line terminator**: `\n` (newline)
- **TLS**: Not supported (AGSsock has no TLS). Traffic is plaintext.

### Message Flow

```
Client                                          Server
  │                                               │
  │──── TELEMETRY|1|<key>|<version>|<platform> ──→│  Handshake
  │                                               │
  │←──────────── OK|<session_id> ─────────────────│  Response
  │                                               │
  │──── 2025-03-23 10:30:01|room_enter|... ──────→│  Event lines
  │──── 2025-03-23 10:30:05|inv_use|... ─────────→│  (one per \n)
  │──── ...                                       │
  │                                               │
  │──── 2025-03-23 11:00:00|session_end|... ─────→│  Final event
  │                                               │
  │──── [connection closed] ─────────────────────→│
```

### 1. Handshake (New Session)

The first message from the client after connecting:

```
TELEMETRY|1|<api_key>|<build_version>|<platform_tag>\n
```

| Field | Description | Example |
|-------|-------------|---------|
| `TELEMETRY` | Literal string identifying a new session handshake | `TELEMETRY` |
| `1` | Protocol version number | `1` |
| `<api_key>` | Authentication key (may be empty string) | `sk_beta_abc123` |
| `<build_version>` | Game version string | `0.0.1-Khrob` |
| `<platform_tag>` | Platform identifier (may be empty string) | `windows` |

**Server should:**

1. Validate the `api_key` against expected keys. If invalid, respond with `ERROR|invalid_api_key\n` and close the connection.
2. Parse the `build_version`. The companion app convention is `<semver>-<tester_name>` (e.g., `0.0.1-Khrob`), but this is not enforced by the game client.
3. Create a new session on the dashboard via `POST /api/collect/session`:
   ```json
   {
     "fullVersion": "0.0.1-Khrob",
     "platform": "windows",
     "runtimeInfo": null
   }
   ```
4. Respond to the client with the session ID:
   ```
   OK|<session_id>\n
   ```
   Where `<session_id>` is the numeric ID returned by the dashboard (e.g., `42`).

### 2. Handshake (Resume Session)

If the client reconnects after a dropped connection:

```
RESUME|1|<api_key>|<session_id>\n
```

| Field | Description | Example |
|-------|-------------|---------|
| `RESUME` | Literal string identifying a session resume | `RESUME` |
| `1` | Protocol version number | `1` |
| `<api_key>` | Authentication key | `sk_beta_abc123` |
| `<session_id>` | Session ID from the original handshake response | `42` |

**Server should:**

1. Validate the `api_key`.
2. Verify the `session_id` exists and has not been ended.
3. Respond with `OK|<session_id>\n` to confirm the resume.
4. Continue accepting event lines for this session.

If the session cannot be resumed (ended, not found, etc.), respond with `ERROR|<reason>\n`.

### 3. Error Response

```
ERROR|<reason>\n
```

| Reason | When to use |
|--------|-------------|
| `invalid_api_key` | API key doesn't match |
| `invalid_version` | Protocol version not supported |
| `session_not_found` | Resume requested for unknown session ID |
| `session_ended` | Resume requested for an already-ended session |
| `server_error` | Internal server error creating session |

The client will disconnect, back off (5s, 10s, 20s, up to 60s), and retry.

### 4. Event Lines

After a successful handshake, the client sends event lines. Each line is a pipe-delimited string terminated by `\n`, in exactly the same format as the telemetry log file:

```
<timestamp>|<event_type>|<key1>=<value1>|<key2>=<value2>|...\n
```

- **Timestamp format**: `YYYY-MM-DD HH:MM:SS` (e.g., `2025-03-23 14:23:45`)
- **Event type**: One of the types listed below
- **Fields**: Zero or more `key=value` pairs, split on the first `=` in each segment

Events arrive in batches (up to 50 per flush, every 5 seconds by default) or individually, depending on game activity.

### 5. Connection Lifecycle

- **One TCP connection = one game session** (typically)
- The client may reconnect mid-session using `RESUME` if the connection drops
- The client sends a `session_end` event as its final event, then closes the connection
- If the connection drops without `session_end`, the session is considered abandoned

## Parsing Event Lines

### Line Format

Split each line on the `|` character:

```
parts[0] = timestamp     (e.g., "2025-03-23 14:23:45")
parts[1] = event_type    (e.g., "room_enter")
parts[2..] = fields      (e.g., "room_id=1", "phase=before_fadein")
```

For each field part, split on the **first** `=` character:

```
"room_id=1"          → key="room_id",    value="1"
"description=a=b=c"  → key="description", value="a=b=c"
```

Parts without `=` should be skipped.

### Special Case: runtime Event

The `runtime` event's `info` field may contain multi-line text from AGS's `System.RuntimeInfo`. In the log file this appears as continuation lines (lines without a timestamp prefix). Over TCP, the `\n` characters are embedded within the event line as literal newlines.

When the `event_type` is `runtime`, the server should be prepared for the `info` value to contain newlines and should buffer accordingly. In practice, the game client sends it as a single line with `info=<multiline text>`, so the TCP framing handles it as one message via `Socket.Send`.

## Event Types Reference

The server should forward all events to the dashboard via `POST /api/collect/events`:

```json
{
  "sessionId": 42,
  "events": [
    {
      "timestamp": "2025-03-23 14:23:45",
      "eventType": "room_enter",
      "fields": { "room_id": "1", "phase": "before_fadein" }
    }
  ]
}
```

Below is the full list of event types and their fields:

### Session Events

| Event Type | Fields | Notes |
|------------|--------|-------|
| `session_start` | `date` | Date in `YYYY-MM-DD` format |
| `session_end` | `session_seconds`, `active_seconds`, `idle_seconds` | Triggers session end on dashboard |
| `build` | `version` | Game version string (e.g., `0.0.1-beta`) |
| `platform` | `tag` | Platform identifier (e.g., `windows`) |
| `runtime` | `info` | AGS engine runtime info (may contain newlines) |

### Player Interaction Events

| Event Type | Fields |
|------------|--------|
| `inv_use` | `item_id`, `item_name`, `target_type`, `target_id`, `target_name`, `default`, `x`, `y` |
| `inv_use_inv` | `item_id`, `item_name`, `target_item_id`, `target_item_name`, `default` |
| `char_interact` | `mode`, `char_id`, `char_name`, `default`, `x`, `y` |

### Room Events

| Event Type | Fields |
|------------|--------|
| `room_enter` | `room_id`, `phase` (`before_fadein` or `after_fadein`) |
| `room_leave` | `room_id`, `phase` (`before_fadeout` or `after_fadeout`) |

### Game State Events

| Event Type | Fields |
|------------|--------|
| `game_saved` | `slot` |
| `game_restored` | `slot` |
| `idle_state` | `idle` (`0` or `1`) |
| `unhandled` | `what`, `type` |
| `error` | `message` |

### Custom Events

| Event Type | Fields |
|------------|--------|
| `custom` | `event`, `data` |
| `milestone` | `name` |

### Bug Reports

| Event Type | Fields |
|------------|--------|
| `bug_report` | `blocking`, `room`, `x`, `y`, `score`, `active_inv`, `screenshot`, `description` |

Note: The `screenshot` field contains a local file path on the player's machine. Screenshots are **not** sent over the TCP connection. They must be collected separately (e.g., via the companion app or manual upload).

## Forwarding to the Dashboard REST API

The TCP listener should translate received data into dashboard API calls. Here is the mapping:

### On New Session Handshake

Call `POST /api/collect/session`:

```http
POST /api/collect/session HTTP/1.1
Content-Type: application/json
Authorization: Bearer <api_key>

{
  "fullVersion": "<build_version from handshake>",
  "platform": "<platform_tag from handshake or null>"
}
```

Expected response:
```json
{
  "sessionId": 42,
  "testerName": "khrob",
  "version": "0.0.1"
}
```

Use the `sessionId` from the response as the `<session_id>` in the `OK` response to the client, and for all subsequent event submissions.

### On Event Lines (Batched)

Collect event lines and periodically flush them via `POST /api/collect/events`:

```http
POST /api/collect/events HTTP/1.1
Content-Type: application/json
Authorization: Bearer <api_key>

{
  "sessionId": 42,
  "events": [
    {
      "timestamp": "2025-03-23 14:23:45",
      "eventType": "room_enter",
      "fields": { "room_id": "1", "phase": "before_fadein" }
    },
    {
      "timestamp": "2025-03-23 14:23:50",
      "eventType": "inv_use",
      "fields": {
        "item_id": "5",
        "item_name": "Key",
        "target_type": "hotspot",
        "target_id": "3",
        "target_name": "Door",
        "default": "0",
        "x": "150",
        "y": "100"
      }
    }
  ]
}
```

Expected response:
```json
{
  "accepted": 2,
  "sessionId": 42,
  "bugIds": []
}
```

Note: All field values are strings, even numeric ones like `"1"` and `"150"`.

### On session_end Event

After forwarding the `session_end` event via the events endpoint, also call `PATCH /api/collect/session/{id}`:

```http
PATCH /api/collect/session/42 HTTP/1.1
Content-Type: application/json
Authorization: Bearer <api_key>

{
  "sessionSeconds": 300,
  "activeSeconds": 280,
  "idleSeconds": 20
}
```

Extract the timing values from the `session_end` event's fields (`session_seconds`, `active_seconds`, `idle_seconds`), converting them from strings to integers.

Expected response:
```json
{
  "sessionId": 42,
  "endedAt": "2025-03-23T15:05:00.000Z"
}
```

### On runtime Event

The `runtime` event contains `System.RuntimeInfo` from AGS. If the TCP listener receives this before creating the server session, include it in the `POST /api/collect/session` request:

```json
{
  "fullVersion": "0.0.1-Khrob",
  "platform": "windows",
  "runtimeInfo": "Adventure Game Studio v3.6.2..."
}
```

Since the game client sends `session_start`, `build`, `platform`, and `runtime` events in rapid succession during `Telemetry_StartSession()`, you may want to buffer the first few events and defer the `POST /api/collect/session` call until you have the `build` event (which contains the version string needed to create the session).

## Implementation Considerations

### Recommended Architecture

A minimal implementation needs:

1. **TCP listener** on the configured port (default 9001)
2. **Per-connection handler** that:
   - Reads the handshake line
   - Authenticates and creates a session
   - Reads subsequent lines and buffers them
   - Periodically flushes buffered events to the dashboard API
   - Handles `session_end` specially (end the session on the dashboard)
3. **Graceful shutdown** handling for interrupted connections

### Buffering Strategy

The game client sends events in bursts (up to 50 lines every 5 seconds). The server should:

- Buffer incoming lines until it has a `build` event (needed to create the dashboard session)
- Once the session is created on the dashboard, flush the buffer
- Continue buffering and flushing periodically (every few seconds)
- On `session_end`, flush immediately before ending the session

### Concurrency

Multiple game clients may connect simultaneously. Each connection should be handled independently with its own session state. Use one thread/task per connection or an async runtime.

### Error Handling

- If the dashboard API is unreachable, buffer events in memory (or a local queue) and retry
- If a client disconnects without sending `session_end`, mark the session as abandoned after a timeout
- Log parsing errors but don't disconnect the client (skip malformed lines)

### Authentication

The `api_key` is sent in plaintext (no TLS). For beta testing this is acceptable. For production use, consider:

- Running the TCP listener on a private network or VPN
- Using a reverse proxy with TLS termination in front of the TCP listener
- Rotating API keys regularly

### Example: Minimal Node.js Server

```javascript
const net = require('net');

const API_KEY = 'your-api-key';
const DASHBOARD_URL = 'http://localhost:3000';

const server = net.createServer((socket) => {
  let buffer = '';
  let sessionId = null;
  let eventQueue = [];

  socket.on('data', (data) => {
    buffer += data.toString();
    const lines = buffer.split('\n');
    buffer = lines.pop(); // Keep incomplete line in buffer

    for (const line of lines) {
      if (line.trim().length === 0) continue;

      if (sessionId === null) {
        // Expecting handshake
        handleHandshake(socket, line.trim())
          .then(id => {
            sessionId = id;
            socket.write(`OK|${id}\n`);
          })
          .catch(err => {
            socket.write(`ERROR|${err.message}\n`);
            socket.end();
          });
      } else {
        // Event line
        const event = parseLine(line.trim());
        if (event) {
          eventQueue.push(event);

          if (event.eventType === 'session_end') {
            flushEvents(sessionId, eventQueue).then(() => {
              endSession(sessionId, event.fields);
            });
            eventQueue = [];
          }
        }
      }
    }
  });

  // Periodic flush
  const flushInterval = setInterval(() => {
    if (sessionId && eventQueue.length > 0) {
      flushEvents(sessionId, eventQueue);
      eventQueue = [];
    }
  }, 5000);

  socket.on('close', () => clearInterval(flushInterval));
  socket.on('error', () => clearInterval(flushInterval));
});

async function handleHandshake(socket, line) {
  const parts = line.split('|');
  const type = parts[0]; // TELEMETRY or RESUME
  const version = parts[1]; // Protocol version
  const apiKey = parts[2];

  if (apiKey !== API_KEY) throw new Error('invalid_api_key');

  if (type === 'RESUME') {
    const sessionId = parts[3];
    // Verify session exists on dashboard
    return sessionId;
  }

  // New session
  const buildVersion = parts[3] || '';
  const platform = parts[4] || '';

  const res = await fetch(`${DASHBOARD_URL}/api/collect/session`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'Authorization': `Bearer ${apiKey}`
    },
    body: JSON.stringify({
      fullVersion: buildVersion,
      platform: platform || undefined
    })
  });

  const data = await res.json();
  return data.sessionId;
}

function parseLine(line) {
  const parts = line.split('|');
  if (parts.length < 2) return null;

  const fields = {};
  for (let i = 2; i < parts.length; i++) {
    const eqIdx = parts[i].indexOf('=');
    if (eqIdx > 0) {
      fields[parts[i].substring(0, eqIdx).trim()] =
        parts[i].substring(eqIdx + 1).trim();
    }
  }

  return {
    timestamp: parts[0].trim(),
    eventType: parts[1].trim(),
    fields
  };
}

async function flushEvents(sessionId, events) {
  if (events.length === 0) return;
  await fetch(`${DASHBOARD_URL}/api/collect/events`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'Authorization': `Bearer ${API_KEY}`
    },
    body: JSON.stringify({ sessionId, events })
  });
}

async function endSession(sessionId, fields) {
  await fetch(`${DASHBOARD_URL}/api/collect/session/${sessionId}`, {
    method: 'PATCH',
    headers: {
      'Content-Type': 'application/json',
      'Authorization': `Bearer ${API_KEY}`
    },
    body: JSON.stringify({
      sessionSeconds: parseInt(fields.session_seconds) || 0,
      activeSeconds: parseInt(fields.active_seconds) || 0,
      idleSeconds: parseInt(fields.idle_seconds) || 0
    })
  });
}

server.listen(9001, () => {
  console.log('Telemetry TCP listener on port 9001');
});
```

This is a starting point. A production implementation should add proper error handling, connection timeouts, event buffering for the initial handshake period (waiting for the `build` event before creating the session), and logging.
