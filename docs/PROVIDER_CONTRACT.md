# Provider Contract (IPC v2.0)

This document defines the **drop-in UI provider contract** for `bb-auth`.

Any executable that follows this contract can act as a provider without daemon code changes.

## Compatibility

- Protocol version: **2.0**
- Transport: **line-delimited JSON** over the daemon local socket
- Backward compatibility target: current daemon behavior in `src/core/Agent.cpp`, `src/core/Session.*`, and `src/core/agent/*`

## Transport and framing

- Connect to daemon socket (`$XDG_RUNTIME_DIR/bb-auth.sock` unless explicitly overridden by launcher args).
- Each message must be one JSON object followed by `\n`.
- Messages are independent; no envelope wrapping.

## Required provider behavior

A conforming provider must:

1. Connect to daemon socket.
2. Send `ui.register` with provider metadata.
3. Send `subscribe`.
4. Send `ui.heartbeat` periodically (recommended <= 4s; daemon timeout is 15s).
5. Handle incoming events:
   - `session.created`
   - `session.updated`
   - `session.closed`
   - `ui.active`
6. Submit user decisions with:
   - `session.respond`
   - `session.cancel`
7. Handle daemon `error` replies; specifically treat `"Not active UI provider"` as authoritative and stop interactive submission while inactive.

## Provider -> Daemon messages

### Register provider

```json
{"type":"ui.register","name":"my-provider","kind":"custom","priority":10}
```

Fields:
- `name` (string): human-readable provider name
- `kind` (string): provider class label
- `priority` (int): higher wins active-provider selection

### Heartbeat

```json
{"type":"ui.heartbeat","id":"<provider-id>"}
```

Notes:
- `id` is returned in `ui.registered`.
- Daemon currently keys heartbeat by socket and tolerates `id` mismatch/absence; providers should still send `id` for compatibility.

### Subscribe to events

```json
{"type":"subscribe"}
```

### Submit response

```json
{"type":"session.respond","id":"<session-id>","response":"secret"}
```

### Cancel session

```json
{"type":"session.cancel","id":"<session-id>"}
```

## Daemon -> Provider messages

### Registration acknowledgement

```json
{"type":"ui.registered","id":"<provider-id>","active":true,"priority":10}
```

Fields:
- `id` (string): daemon-assigned provider id
- `active` (bool): whether this provider is currently active
- `priority` (int): accepted provider priority

### Subscription acknowledgement

```json
{"type":"subscribed","sessionCount":1,"active":true}
```

Fields:
- `sessionCount` (int): number of interactive sessions sent to this socket
- `active` (bool, optional): active-provider state for registered providers

### Active provider status broadcast

```json
{"type":"ui.active","active":true,"id":"<provider-id>","name":"...","kind":"...","priority":10}
```

When no active provider exists:

```json
{"type":"ui.active","active":false}
```

### Session created

```json
{
  "type":"session.created",
  "id":"<session-id>",
  "source":"polkit|keyring|pinentry",
  "context":{
    "message":"...",
    "requestor":{
      "name":"...",
      "icon":"...",
      "fallbackLetter":"A",
      "fallbackKey":"optional",
      "pid":1234
    },
    "actionId":"optional (polkit)",
    "user":"optional (polkit)",
    "details":{},
    "keyringName":"optional (keyring)",
    "description":"optional (pinentry)",
    "keyinfo":"optional (pinentry)",
    "curRetry":0,
    "maxRetries":3,
    "confirmOnly":false,
    "repeat":false
  }
}
```

Notes:
- `context.requestor` fields may be empty/missing except `name` fallback defaults to `"Unknown"`.
- Source-specific context fields appear only when relevant.

### Session updated

```json
{
  "type":"session.updated",
  "id":"<session-id>",
  "state":"prompting",
  "prompt":"...",
  "echo":false,
  "curRetry":0,
  "maxRetries":3,
  "error":"optional",
  "info":"optional"
}
```

Notes:
- `curRetry` and `maxRetries` are present for pinentry sessions.
- `error` and `info` are optional, transient display fields.

### Session closed

```json
{"type":"session.closed","id":"<session-id>","result":"success|cancelled|error","error":"optional"}
```

### Generic replies

```json
{"type":"ok", ...}
{"type":"error","message":"..."}
{"type":"pong", ...}
```

## Routing and authorization semantics (must preserve)

- Session events are routed to:
  - active provider socket (if one exists), and
  - explicit `next` waiters.
- Non-session events (`ui.active`, etc.) are broadcast to subscribers.
- If a provider is not active, `session.respond` / `session.cancel` must be rejected with:

```json
{"type":"error","message":"Not active UI provider"}
```

Providers must treat this as an authorization boundary, not a transient warning.

## Lifecycle expectations

1. Connect.
2. Register.
3. Subscribe.
4. Process `ui.active` transitions.
5. Render interactive sessions only while active.
6. Heartbeat continuously while connected.
7. On disconnect/error, reconnect with backoff and repeat registration/subscription.

## Conformance notes

- The in-tree Qt fallback (`src/fallback/*`) is a reference provider implementation.
- Additional providers are expected to live out-of-tree and integrate via `providers.d` manifests.
- Future protocol revisions must update this document and preserve compatibility guarantees for v2 providers where possible.
