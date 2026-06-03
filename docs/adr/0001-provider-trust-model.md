# 0001 — Provider trust by daemon-launch attestation

Status: accepted (2026-06-02)

## Context

`bb-auth` routes the user's secret (a sudo/polkit password, a keyring unlock, a pinentry
PIN) to whichever registered UI provider is *active*. The IPC socket is a Unix domain
socket with `QLocalServer::UserAccessOption` — mode 0600 — so every peer is a process
running as the **same user**. The realistic adversary for a daemon whose pitch is
*supervising agents* is therefore a hostile same-UID process: a compromised app, or a
rogue AI agent.

Under the inherited v2.x contract, becoming the active provider was unauthenticated:

- `ui.register` accepted an unbounded `priority`; any socket could send
  `priority: 2147483647`, win active-provider selection, and become the sink for the
  secret.
- `isAuthorized` returned true for *any* socket when no providers were registered — an open
  window before the real UI registered.

These let a same-UID process silently intercept the password. That voids the product
promise, so it had to change even though the fix breaks the frozen v2.x provider contract.

## Decision

A provider may become active (and thus receive the secret and session events) **only if it
was launched by the daemon itself**, proven by kernel-attested peer credentials:

- When the daemon launches a provider (`ProviderLauncher`), it captures the child pid
  (`QProcess::startDetached(&pid)`) and records `{pid, start-time}` (`ProviderTrustStore`).
- At `ui.register`, the daemon reads the connection's `SO_PEERCRED` pid and matches it
  against the launch records. Start-time defeats pid reuse; the match is single-use.
- `ProviderRegistry::isAuthorized` requires a *trusted + active* provider and no longer has
  an empty-registry allow — it fails closed. The daemon launches its own (trusted) fallback
  whenever a session needs a provider, so there is never a no-provider window.
- Requested `priority` is clamped to `[-1000, 1000]` as defense-in-depth.

Session events follow the same trust boundary: they are delivered only to the active
provider, never broadcast to other subscribers or to `next` pull-waiters.

## Consequences

- **Breaking (IPC v3.0).** A free-standing third-party provider that registered under v2.x
  stops being authorized. Providers must be daemon-launched (autostart manifest, or the
  built-in fallback). A provider must connect directly from the launched process — a
  fork-then-connect from an unrelated pid will not attest.
- The custom UI (Quickshell, Workstream C) must be built as a **daemon-launchable prompt
  surface**, not a free-standing shell widget that owns password entry. This matches the
  macOS "trusted system UI" model.
- No secret is introduced. We rejected an environment-passed launch token because a
  same-UID adversary can read it from `/proc/<pid>/environ`; `SO_PEERCRED` cannot be forged
  and needs no secret, and it reuses the daemon's existing pid + start-time machinery.

## Alternatives considered

- **Per-launch env token** (`BB_AUTH_PROVIDER_TOKEN`): simpler and launch-mechanism
  agnostic, but leaks to the exact adversary via `/proc/<pid>/environ`. Rejected.
- **Ancestry walk** (trust any descendant of a launched pid): defeated anyway by Qt's
  `startDetached` double-fork + `setsid` (the launched process reparents to `init`), and it
  would *add* an attack surface for providers that spawn subprocesses. Rejected in favor of
  direct pid match.
- **Keep a compatibility shim** for free-standing providers: reintroduces the open window.
  Rejected; the breaking change is the point.

## Residual limit

Perfect isolation between same-UID processes is not achievable without separate OS
credentials (they can `ptrace`/impersonate one another on default setups). This model
raises the bar as far as a single same-UID daemon can. A separate trusted identity (the
macOS SecurityAgent model) is the longer-term direction, out of scope here.
