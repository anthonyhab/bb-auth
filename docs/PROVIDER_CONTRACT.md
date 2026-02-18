# Provider Contract (IPC v2.0)

Status: locked (2026-02-18)

This document defines the runtime contract for external UI providers that integrate with `bb-auth`.
Any executable can be a provider as long as it follows this protocol.

## 1. Scope

This contract covers:

- provider-to-daemon IPC messages
- daemon-to-provider IPC messages
- provider selection and authorization semantics
- compatibility and versioning policy

Provider packaging details are in `docs/PROVIDER_PACKAGING.md`.

## 2. Normative language

The key words `MUST`, `MUST NOT`, `SHOULD`, `SHOULD NOT`, and `MAY` are to be interpreted as normative requirements.

## 3. Compatibility and versioning

- Protocol version: `2.0`.
- Versioning policy:
  - `2.x` changes MUST be backward-compatible and additive.
  - Removing fields, changing field meaning, or changing authorization behavior requires a major version bump.
- Providers MUST ignore unknown fields in daemon messages.
- Daemon behavior for unknown provider message types is an `error` reply with message `Unknown type`.

## 4. Transport and framing

- Transport is a Unix domain stream socket (`QLocalSocket`/`QLocalServer`).
- Default daemon socket path is `$XDG_RUNTIME_DIR/bb-auth.sock` unless overridden with `--socket`.
- Framing is newline-delimited JSON:
  - each message is one JSON object followed by `\n`
  - UTF-8 encoding
  - no outer envelope
- Maximum message size is `64 KiB`; larger buffered input disconnects the client.
- Invalid JSON yields:
  - `{"type":"error","message":"Invalid JSON"}`
- Missing `type` yields:
  - `{"type":"error","message":"Missing type field"}`

## 5. Provider lifecycle state machine

Expected state progression:

1. `Connected`
2. `Registered` (`ui.register`)
3. `Subscribed` (`subscribe`)
4. `Active` or `Inactive` (based on daemon provider selection)
5. `Disconnected` (then reconnect with backoff)

Lifecycle requirements:

- Provider MUST register after connecting.
- Provider MUST subscribe to receive routed events.
- Provider MUST send heartbeat periodically while connected.
- Provider SHOULD reconnect with bounded exponential backoff after disconnect/error.

## 6. Registration and active-provider selection

Provider -> daemon registration:

```json
{"type":"ui.register","name":"my-provider","kind":"custom","priority":10}
```

Registration fields:

| Field | Type | Required | Notes |
|---|---|---|---|
| `type` | string | yes | must be `ui.register` |
| `name` | string | no | default is `unknown` |
| `kind` | string | no | default is `name`, then `unknown` |
| `priority` | int | no | default depends on `kind` |

Default priority behavior:

- `kind == "quickshell"` -> `100`
- `kind == "fallback"` -> `10`
- all other kinds -> `50`

Daemon replies with:

```json
{"type":"ui.registered","id":"<provider-id>","active":true,"priority":10}
```

Provider selection algorithm:

1. Disconnected providers are pruned.
2. Providers stale for more than `15000 ms` since register/heartbeat are pruned.
3. Highest priority wins.
4. Priority ties break by most recent heartbeat timestamp.
5. If priority and heartbeat are equal, selection is implementation-defined; providers SHOULD avoid equal-priority contention.

## 7. Heartbeat contract

Provider heartbeat:

```json
{"type":"ui.heartbeat","id":"<provider-id>"}
```

Notes:

- `id` is optional in current daemon behavior (socket identity is authoritative), but providers SHOULD send it.
- Heartbeat SHOULD be sent at least every 4 seconds.
- Missing heartbeat for more than 15 seconds can cause provider pruning and active-provider loss.

Heartbeat responses:

- Success:
  - `{"type":"ok","active":<bool>}`
- Failure (unregistered socket):
  - `{"type":"error","message":"Provider not registered"}`

## 8. Subscription and event delivery

Subscribe message:

```json
{"type":"subscribe"}
```

Daemon reply:

```json
{"type":"subscribed","sessionCount":1,"active":true}
```

Field semantics:

- `sessionCount`: number of current interactive sessions visible to that socket.
- `active`: included for registered providers, indicates active-provider state.

Routing semantics:

- Session events (`session.created`, `session.updated`, `session.closed`) are routed to:
  - active provider socket (if present), and
  - explicit `next` waiters.
- Non-session events (for example `ui.active`) are broadcast to subscribers.
- A provider socket is not implicitly subscribed by registration; it SHOULD call `subscribe`.

## 9. Interactive session API

Respond:

```json
{"type":"session.respond","id":"<session-id>","response":"secret"}
```

Cancel:

```json
{"type":"session.cancel","id":"<session-id>"}
```

Authorization and safety:

- If provider authorization fails:
  - `{"type":"error","message":"Not active UI provider"}`
- Providers MUST treat this as authoritative and MUST stop interactive submission until active again.

Other error examples:

- `Unknown session`
- `Session is not accepting input`
- `Session is not awaiting direct response`

## 10. Authorization model

Primary rule:

- When one or more providers are registered, only the active provider is authorized to submit `session.respond`/`session.cancel`.

Legacy compatibility mode:

- If no providers are registered, daemon currently authorizes any socket for interactive submission.
- New providers SHOULD always register so active-provider boundaries are enforced.

## 11. Daemon -> provider message summary

`ui.registered`:

```json
{"type":"ui.registered","id":"<provider-id>","active":true,"priority":10}
```

`ui.active` with active provider:

```json
{"type":"ui.active","active":true,"id":"<provider-id>","name":"...","kind":"...","priority":10}
```

`ui.active` with no active provider:

```json
{"type":"ui.active","active":false}
```

`session.created` / `session.updated` / `session.closed` payloads remain as defined by current daemon session model in `src/core/Session.*`.

Generic replies:

```json
{"type":"ok", ...}
{"type":"error","message":"..."}
{"type":"pong", ...}
```

## 12. Security expectations

- Providers MUST treat all incoming JSON as untrusted input.
- Providers SHOULD avoid logging secrets from prompts/responses.
- Providers SHOULD fail closed (cancel or no-op) on malformed session data.
- Providers SHOULD handle `ui.active` transitions immediately to avoid unauthorized submission attempts.

## 13. Conformance suite mapping

Contract-level checks are implemented in:

- `tests/test_provider_conformance.cpp`
- `tests/test_ipc_contract.cpp`
- `tests/test_agent_routing.cpp`
- `tests/test_provider_manifest.cpp`
- `tests/test_provider_discovery.cpp`
- `tests/test_provider_launcher.cpp`

Current explicit coverage includes:

- provider-dir precedence and dedupe semantics
- registration defaults and priority behavior
- heartbeat rejection for unregistered sockets
- tie-break by latest heartbeat
- stale-provider pruning after timeout
- active-provider authorization boundary
- session/non-session routing behavior
- invalid JSON and missing `type` framing errors
- unknown message type error behavior
- oversized buffered input disconnect behavior

## 14. Lock checklist

Checklist for keeping this contract locked:

1. All conformance tests MUST pass in CI.
2. Core-only and provider-template CI paths MUST pass.
3. Any proposed protocol change MUST include:
   - contract diff
   - compatibility impact statement
   - conformance test updates

If this checklist is no longer satisfied, status SHOULD revert to `lock candidate` until resolved.
