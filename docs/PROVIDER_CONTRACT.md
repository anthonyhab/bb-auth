# Provider Contract

This document describes the IPC protocol for shell providers (Waybar, ags, custom widgets) that want to display bb-auth prompts in their UI.

## Overview

**Architecture**
- bb-auth daemon runs as a systemd user service
- Shell providers connect via Unix domain socket
- JSON messages over newline-delimited stream
- Providers register themselves with priority
- Highest priority active provider receives all prompt events

**Socket Location**
```
$XDG_RUNTIME_DIR/bb-auth.sock
```

## Connection

Providers connect to the daemon using local Unix sockets. The daemon accepts multiple concurrent connections.

```python
# Python example
import socket
import json

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'bb-auth.sock'))
```

## Message Format

All messages are JSON objects, one per line, newline-delimited.

```json
{"type": "ping"}
```

## Provider Registration

Providers must register to receive prompt events.

### Register

Request:
```json
{
  "type": "ui.register",
  "name": "MyShell",
  "kind": "waybar-widget",
  "priority": 10
}
```

Fields:
- `name`: Human-readable provider name
- `kind`: Provider type (e.g., "waybar-widget", "ags", "custom")
- `priority`: Integer, higher = preferred (default: 0)

Response:
```json
{
  "type": "ui.registered",
  "id": "uuid-here",
  "active": true,
  "priority": 10
}
```

### Heartbeat

Providers must heartbeat every ~2 seconds to stay alive:

```json
{"type": "ui.heartbeat"}
```

Response:
```json
{
  "type": "ok",
  "active": true
}
```

**Important**: Stale providers (no heartbeat for 10+ seconds) are automatically pruned.

### Unregister

Clean disconnect:
```json
{"type": "ui.unregister"}
```

## Receiving Events

Subscribe to session events:

```json
{"type": "subscribe"}
```

Response:
```json
{
  "type": "subscribed",
  "sessionCount": 1,
  "active": true
}
```

You'll receive events for all active sessions and new sessions as they are created.

### Event Types

#### Session Created
```json
{
  "type": "session.created",
  "id": "cookie-here",
  "source": "polkit",
  "message": "Authentication required",
  "actionId": "org.freedesktop.policykit.exec",
  "user": "root",
  "requestor": {
    "name": "pkexec",
    "icon": "dialog-password",
    "fallbackLetter": "P",
    "fallbackKey": "pkexec",
    "pid": 12345
  }
}
```

Sources: `polkit`, `keyring`, `pinentry`

#### Session Updated
```json
{
  "type": "session.updated",
  "id": "cookie-here",
  "prompt": "Password:",
  "echo": false,
  "error": null
}
```

#### Session Closed
```json
{
  "type": "session.closed",
  "id": "cookie-here",
  "result": "success"
}
```

Results: `success`, `cancelled`, `error`

#### Provider Status
```json
{
  "type": "ui.active",
  "active": true,
  "id": "uuid",
  "name": "MyShell",
  "kind": "waybar-widget",
  "priority": 10
}
```

## Responding to Prompts

When you're the active provider, you can respond to sessions:

### Submit Response
```json
{
  "type": "session.respond",
  "id": "cookie-here",
  "response": "user-password"
}
```

Response:
```json
{"type": "ok"}
```

### Cancel Session
```json
{
  "type": "session.cancel",
  "id": "cookie-here"
}
```

Response:
```json
{"type": "ok"}
```

## Keyring and Pinentry

### Keyring Request
```json
{
  "type": "keyring_request",
  "operation": "unlock",
  "keyring": "login"
}
```

### Pinentry Request
```json
{
  "type": "pinentry_request",
  "title": "Unlock key",
  "description": "Enter passphrase",
  "prompt": "Passphrase:"
}
```

## Utility Messages

### Ping
```json
{"type": "ping"}
```

Response:
```json
{
  "type": "pong",
  "version": "2.0",
  "capabilities": ["polkit", "keyring", "pinentry", "fingerprint", "fido2"],
  "provider": {
    "id": "uuid",
    "name": "MyShell",
    "kind": "waybar-widget",
    "priority": 10
  },
  "bootstrap": {
    "timestamp": 1234567890,
    "mode": "session",
    "pinentry_path": "/usr/libexec/pinentry-bb"
  }
}
```

### Next Event (Polling)
For providers that prefer polling over subscription:
```json
{"type": "next"}
```

If no events are pending, the daemon will hold the request until one arrives.

## Priority System

Multiple providers can connect simultaneously. The active provider is determined by:

1. **Priority**: Highest `priority` value wins
2. **Recency**: If tied, most recent heartbeat wins
3. **Pruning**: Providers without heartbeat for 10+ seconds are removed

When the active provider disconnects, the next highest priority provider becomes active automatically. If no providers are active, the fallback window launches.

## Security Notes

- Socket uses peer credential checking (SO_PEERCRED)
- Only the active provider can submit responses
- Password data is securely zeroed in memory
- Each session has a unique cookie ID

## Example Flow

```
1. Provider connects to socket
2. Provider sends: {"type": "ui.register", "name": "MyBar", "priority": 10}
3. Daemon responds: {"type": "ui.registered", "id": "...", "active": true}
4. Provider sends: {"type": "subscribe"}
5. User runs: pkexec echo "test"
6. Daemon sends: {"type": "session.created", "id": "cookie1", ...}
7. Daemon sends: {"type": "session.updated", "id": "cookie1", "prompt": "Password:"}
8. Provider displays prompt UI
9. User enters password
10. Provider sends: {"type": "session.respond", "id": "cookie1", "response": "secret"}
11. Daemon sends: {"type": "session.closed", "id": "cookie1", "result": "success"}
12. Provider hides prompt UI
```

## Reference Implementations

- **Noctalia Shell**: [bb-auth plugin](https://github.com/anthonyhab/noctalia-plugins/)

## Version History

- **2.0** (Current): Full provider contract with priority system
- **1.0**: Basic subscription model

