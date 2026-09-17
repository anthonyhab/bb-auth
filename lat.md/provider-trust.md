# Provider trust model

Only a provider the daemon itself launched may become active and receive the secret; trust is proven by kernel-attested peer credentials, never by anything the peer asserts. (Living spec of ADR 0001.)

## Why same-UID is the adversary

The IPC socket is `QLocalServer::UserAccessOption` (mode 0600), so every peer is a same-UID process: a compromised app or a rogue AI agent could silently intercept the password.

Under the v2.x contract `ui.register` accepted an unbounded `priority` (a hostile socket could win active-provider selection with `INT32_MAX`) and `isAuthorized` allowed *any* socket when no providers were registered. Both voided the product promise — supervising agents — so v3.0 broke the frozen contract.

## Daemon-launch attestation

Becoming active requires a launch attestation: when the daemon spawns a provider it records `{pid, start-time}`, then at `ui.register` reads the socket's `SO_PEERCRED` pid and demands an exact match.

- Recorded at launch by `ProviderLauncher` (`src/core/providers/`; `startDetached` → child pid, `/proc` start-time read).
- Matched single-use by `ProviderTrustStore` (`src/core/agent/`) — start-time defeats pid reuse, and a consumed record can never attest a second registration.
- The ancestry walk was dropped: only a *direct* pid match is trusted. This is why the omarchy prompt wrapper must `exec` (preserve pid) — see [[omarchy-prompt#Exit on disconnect]].

## Fail-closed authorization

`ProviderRegistry::isAuthorized` requires a *trusted + active* provider; there is no empty-registry allow.

The daemon always launches its own trusted fallback when a session needs a provider, so a no-provider window cannot exist — fail-closed never means fail-locked-out.

## Priority clamp

Requested `priority` is clamped to `[-1000, 1000]` as defense-in-depth: even a trusted provider cannot dominate selection by assertion.

## Event delivery boundary

Session events are delivered only to the active provider — never broadcast to other subscribers or `next` pull-waiters — so an untrusted registered socket cannot siphon secret-bearing events.

## Residual limit

Perfect isolation between same-UID processes is not achievable without separate OS credentials; this model is the strongest guarantee available inside one UID.
