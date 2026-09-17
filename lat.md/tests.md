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
