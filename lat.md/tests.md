---
lat:
  require-code-mention: true
---
# Test specifications

Security-critical guarantees that must stay covered: every leaf spec maps to exactly one Qt test via a `// @lat:` comment.

## Provider trust

Attestation semantics enforced by `ProviderTrustStore` (`src/core/agent/`) — the checks that keep a hostile same-UID process from ever becoming active ([[provider-trust#Provider trust model]]).

### Trusts launched peer by pid and start-time

A registration whose `SO_PEERCRED` pid matches a recorded launch *and* whose `/proc` start-time matches is trusted — the positive case every other check defends.

### Rejects unknown peer

A socket with no matching launch record is untrusted, regardless of what it claims — assertion never substitutes for attestation.

### Recycled pid fails closed

A pid that matches a launch record but whose start-time differs (reused pid) is rejected — pid alone is never sufficient proof.

### Trust attestation is single-use

A consumed launch record cannot attest a second registration — a trusted process cannot hand its attestation to a successor.

## Intent correlation

Binding rules enforced by `IntentStore` (`src/core/agent/`) — declared intent must attach to the *resolved* agent, never the self-asserted one ([[agent-intent#Intent declaration and correlation]]).

### Correlates by resolved agent and start-time

A declared intent attaches to the subsequent request only when the declarer's resolved agent ancestry ({pid, start-time}) matches.

### Start-time mismatch fails closed

A declaration whose pid matches but start-time differs never correlates — recycled pids cannot inherit another process's declared reason.

### Consume is one-shot

A correlated intent is consumed by the first matching request — it cannot be replayed onto subsequent escalations.

### Unbound intent ignored

A declaration that never correlates to a request expires harmlessly — stray declares cannot poison later unrelated prompts.

## Fuzz boundaries

Deterministic seeded fuzz at the two trust boundaries — `parseProviderManifest` (`src/core/providers/`) and `IpcServer` frames (`src/core/ipc/`). Failures reproduce byte-for-byte from the fixed seeds in `tests/test_fuzz_inputs.cpp`.

### Manifest mutations never crash

1500 seeded mutations of a valid manifest (truncate/flip/insert/splice/duplicate) must return a structured `ParseResult` — ok implies `isValid()`, failure implies a non-empty error.

### Adversarial manifest structures bounded

Deep nesting, huge strings/numbers, lone surrogates, duplicate keys, and comma floods must parse or fail bounded — no hang, no crash.

### Garbage IPC frames never wedge server

Random byte frames interleaved with pings on one connection must each yield a reply and the ping must still answer pong — the daemon survives hostile input on a live socket.

### Adversarial IPC frames handled

Non-object JSON, null/numeric/empty/duplicate `type`, padded and escaped frames must each produce a bounded response — error or dispatch — with the connection staying usable.

## Agent handoff

Guarantees of the harness handoff ([[agent-intent#Challenge-gated approval]]) — driven by the Python tests against the built hook, the installer, and each translator.

### Shared translation vectors

Every row of `tests/fixtures/escalation-translation.json` must produce the same rewrite (or none) from the hook as from aisudo, pi, and opencode — translator drift fails the build.

### Silent polkit authorization keeps the harness gate

When `pkcheck` reports anything but a challenge (YES rule, denial, error) the hook emits no rewrite and no "allow" — the harness never waves through a command polkit would not stop.

### Post-run annotation

Claude `PostToolUse` on pkexec yields a truthful `classifierContext`, 126/127 failures yield decline guidance, unrelated failures and Codex yield nothing, and post events never declare.

### Installer is idempotent and ownership-bounded

`bb-auth-agents install` twice is byte-identical, keeps unrelated settings, and `uninstall` restores the original config exactly.

## Daemon lifecycle

Socket ownership rules enforced by `IpcServer::start` (`src/core/ipc/`) — a second daemon must never steal a live agent's socket path.

### Live socket never hijacked

Starting on a path owned by a live daemon must fail closed — the running agent keeps its socket and its polkit registration; the contender exits.

### Stale socket reclaimed

A socket file left by a dead daemon must be removed and rebound — a crash must not wedge future starts.
