# Provider Contract (IPC v3.0)

Status: locked (2026-02-18); v2.1 additive extension (2026-06-02); v3.0 trust-model change (2026-06-02)

This document defines the runtime contract for external UI providers that integrate with `bb-auth`.

> **v3.0 is a breaking change to the authorization model.** Only a provider the daemon
> itself launched may become the active provider that receives the user's secret. A
> free-standing provider that the daemon did not spawn can connect and register, but can
> never become active and is never authorized to submit `session.respond`/`session.cancel`.
> See §10 and `docs/adr/0001-provider-trust-model.md`. Providers that ran as free-standing
> processes under v2.x must migrate to being daemon-launched.

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

- Protocol version: `3.0` (advertised in the `pong` `version` field).
- Versioning policy:
  - Minor (`x.y`) changes MUST be backward-compatible and additive.
  - Removing fields, changing field meaning, or changing authorization behavior requires a major version bump.
  - `3.0` is a major bump because it changes authorization behavior (see §10): the legacy
    empty-registry allow was removed and active-provider trust now requires daemon launch.
- Providers MUST ignore unknown fields in daemon messages.
- Daemon behavior for unknown provider message types is an `error` reply with message `Unknown type`.

### 3.1 v2.1 additions (agent attribution and declared intent)

v2.1 adds, additively:

- `context.requestor.isAgent` / `context.requestor.agentKind` on `session.created` —
  OS-resolved recognition that the requesting process is a known AI agent runtime.
- `context.intent` on `session.created` — a self-asserted reason for the privileged
  action, declared by the agent via the `intent.declare` message.
- The `intent.declare` provider→daemon message (see §9.1).

Trust boundary (normative): `context.intent` and any declared agent id are
**self-asserted and display/audit only**. Providers and the daemon MUST NOT use them
in any authorization or allow/deny decision. The authoritative identity is
`context.requestor` (resolved by the daemon from OS process ancestry). When the daemon
correlates a declared intent whose agent id disagrees with the OS-resolved
`agentKind`, it sets `context.intent.mismatch = true`; providers SHOULD surface this
discrepancy rather than hide it.

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
| `priority` | int | no | default depends on `kind`; clamped to `[-1000, 1000]` |

Default priority behavior:

- `kind == "quickshell"` -> `100`
- `kind == "fallback"` -> `10`
- all other kinds -> `50`

A supplied `priority` is clamped to `[-1000, 1000]`. An out-of-range value (including
`INT_MAX`) cannot be used to force active-provider selection.

Registration succeeds for any connected socket, but **only a daemon-launched (attested)
provider is eligible to become active** (§10). An unattested registration is retained but
never selected as active and never authorized.

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

- Session events (`session.created`, `session.updated`, `session.closed`) carry prompt
  text and the requestor's identity. As of v3.0 they are routed **only to the active
  (attested) provider** — never broadcast to other subscribers, and never delivered to
  `next` pull-waiters. When no provider is active they are not delivered live; the active
  provider that registers later receives open sessions via `subscribe` replay.
- On `subscribe`, the open-session replay (and a non-zero `sessionCount`) is delivered
  **only to the active provider**. A non-active or non-provider subscriber receives
  `sessionCount: 0` and no session replay.
- Non-session events (for example `ui.active`) are broadcast to subscribers and available
  to `next` waiters.
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

### 9.1 Intent declaration (`intent.declare`)

Declares, ahead of a privileged command, why it is being run. Sent by an agent-side
client (a Claude Code PreToolUse hook, or a lean MCP server) on the same socket, BEFORE
the command triggers polkit:

```json
{"type":"intent.declare","reason":"remove duplicate .desktop file","agent":"claude-code","command":"pkexec rm '/usr/share/applications/X.desktop'","cwd":"/home/u/proj","channel":"hook","ttlMs":20000}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `type` | string | yes | must be `intent.declare` |
| `reason` | string | yes | human-readable justification (display/audit only; truncated to 2000 chars) |
| `agent` | string | no | self-asserted agent id (untrusted; cross-checked against OS resolution) |
| `command` | string | no | command text (display only; truncated) |
| `cwd` | string | no | working directory (display only) |
| `channel` | string | no | `hook` or `mcp` |
| `ttlMs` | int | no | validity window; clamped to `[0, 60000]`, default `20000` |

Daemon reply:

```json
{"type":"ok","bound":true}
```

Correlation and trust semantics:

- The daemon binds the declaration to the declarer's **OS-resolved agent ancestry**
  (the agent process the declarer descends from), NOT to the self-asserted `agent`
  field. It correlates to a later polkit request when that request's subject resolves
  to the **same agent process** (matched by pid AND start-time, so a recycled pid fails
  closed). `bound:false` means the declarer had no recognized agent ancestry and the
  intent was discarded.
- An intent is consumed on first correlation (one reason per command) and expires after
  its TTL.
- The declaration NEVER affects the authorization decision. See §3.1.

## 10. Authorization model

Threat model: the IPC socket is `UserAccessOption` (mode 0600), so any peer is a process
running as the **same user**. A hostile same-UID process (a compromised app or a rogue
agent) is the adversary. The active provider receives the user's secret, so becoming the
active provider must not be something an arbitrary same-UID peer can do.

Primary rule:

- Only the **active provider** is authorized to submit `session.respond`/`session.cancel`
  and to receive session events.
- A provider becomes active only if it is **attested as daemon-launched** (see below).
  Registering, sending heartbeats, or claiming a high `priority` is not sufficient.

Provider trust (attestation):

- When the daemon launches a provider (the built-in fallback, or any autostart manifest
  provider), it records that process's pid and start-time.
- At `ui.register`, the daemon reads the connection's kernel-attested peer credentials
  (`SO_PEERCRED`) and matches the peer pid against its launch records. The start-time
  guards against pid reuse; the match is single-use. A match marks the registration
  *trusted*.
- `SO_PEERCRED` cannot be forged by a same-UID peer, and there is no secret to leak (unlike
  an environment-passed token, which a same-UID peer could read from `/proc/<pid>/environ`).
- Consequence for providers: a provider MUST connect directly from the process the daemon
  launched (no fork-then-connect from an unrelated process). A free-standing provider the
  daemon did not launch will register but never become active.

Fail-closed (no legacy mode):

- The v2.x "if no providers are registered, any socket is authorized" mode is **removed**.
  With no active provider, no socket is authorized. The daemon launches its own trusted
  fallback whenever a session needs a provider, so there is never a window in which an
  arbitrary socket can answer a prompt.

Residual (honest limit): perfect isolation between same-UID processes is not achievable
without separate OS credentials (they can `ptrace`/impersonate one another on default
setups). This model raises the bar as far as a single same-UID daemon can; a separate
trusted identity (the macOS SecurityAgent model) is the longer-term direction.

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

`session.created` / `session.updated` / `session.closed` payloads are defined by the daemon session model in `src/core/Session.*`. As of v2.1, `session.created` `context` carries (all optional, omit-when-empty):

```json
{
  "type":"session.created","id":"<id>","source":"polkit",
  "context":{
    "message":"Authentication required",
    "requestor":{"name":"Claude Code","pid":12345,"isAgent":true,"agentKind":"claude-code","fallbackLetter":"C"},
    "intent":{"reason":"remove duplicate .desktop file","declaredAgent":"claude-code","channel":"hook","mismatch":false}
  }
}
```

`requestor` is OS-resolved (authoritative). `intent` is self-asserted (display/audit only). Either may be absent.

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
- `tests/test_provider_trust.cpp`
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
- daemon-launch attestation: trusted launch matched, unknown peer rejected, recycled-pid
  rejected, single-use, abandoned-launch pruning
- untrusted provider never becomes active or authorized
- priority clamped to `[-1000, 1000]`
- session/non-session routing behavior, and session events withheld when no active provider
- invalid JSON and missing `type` framing errors
- unknown message type error behavior
- oversized buffered input disconnect behavior
- incomplete-frame (slow-drip) idle timeout disconnect
- oversized provider manifest skipped before read

## 14. Lock checklist

Checklist for keeping this contract locked:

1. All conformance tests MUST pass in CI.
2. Core-only and provider-template CI paths MUST pass.
3. Any proposed protocol change MUST include:
   - contract diff
   - compatibility impact statement
   - conformance test updates

If this checklist is no longer satisfied, status SHOULD revert to `lock candidate` until resolved.
