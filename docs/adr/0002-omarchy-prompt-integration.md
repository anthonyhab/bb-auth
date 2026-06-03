# 0002 — Omarchy prompt as a resident daemon-launched provider

Status: accepted (2026-06-03)

## Context

ADR 0001 established that only a daemon-launched provider (attested by SO_PEERCRED pid +
start-time) may become active and receive the secret. The omarchy desktop's polkit UI was
`shell/plugins/polkit/PolkitAgent.qml` — a service plugin running *inside* the user-launched
`quickshell -p shell` process, i.e. a free-standing provider. Under v3.0 it registers but is
never trusted, so prompts would fall through to the built-in `bb-auth-fallback` window and
the user would lose their themed prompt.

We own both ends, so the omarchy prompt becomes a first-class **daemon-launched** surface:
same look (it reuses omarchy's design system), trusted by construction.

Two risks had to be retired before this was known to work; both were proven empirically
(isolated probes first, then the live daemon):

1. **Does the connecting pid match the launched pid?** The daemon trusts only a *direct* pid
   match (the ancestry walk was dropped — ADR 0001). The launch chain is daemon → wrapper →
   `exec quickshell`. `exec` preserves the pid, and `quickshell -p` does **not** fork a child
   to own the socket, so SO_PEERCRED reports the launched pid. Direct match holds.
2. **Does a daemon-launched quickshell survive the daemon's systemd sandbox?** `bb-auth.service`
   is hardened (`MemoryDenyWriteExecute=true`, `SystemCallFilter=@system-service`,
   `ProtectSystem=strict`, `PrivateTmp=yes`, empty `CapabilityBoundingSet`). A `startDetached`
   child inherits that sandbox — the double-fork reparents to init but does not escape the
   cgroup, namespaces, or seccomp filter. The existing fallback is Qt **Widgets** (no QML
   JIT), so nothing had proven a QML app survives. It does: quickshell runs under the exact
   hardening, the V4 engine executes under `MemoryDenyWriteExecute` (JIT falls back to the
   interpreter), and the layer-shell window renders. **No hardening relaxation is needed.**

## Decision

- **Resident, daemon-launched prompt.** The daemon eager-launches the highest-priority
  autostart provider at startup (after the IPC server binds) and relaunches it on disconnect,
  regardless of pending sessions (`ProviderLauncher::tryLaunch(..., eager=true)`,
  `CAgent::ensureFallbackUiRunning(reason, eager)`). The prompt stays hidden until a session
  arrives, giving zero first-prompt latency. Eager launch **only ever selects a real
  autostart provider** — never the legacy env override or the built-in fallback, which remain
  on-demand safety nets. With no autostart provider configured the daemon stays fully
  on-demand (current behaviour preserved).

- **Reuse the design system by import path, no duplication.** The entry is
  `shell/bb-prompt.qml`, launched `quickshell -p <shell-dir>/bb-prompt.qml`. quickshell
  resolves the `qs.*` import prefix from the config-root directory (the dir holding the
  launched file), so config root = the omarchy shell dir and `qs.Commons`/`qs.Ui` resolve.
  There is **no `-I` flag** in quickshell. The `Style`/`Color` singletons self-initialise
  from `~/.config/omarchy/current/theme/*.toml` on construction, so a standalone process is
  correctly themed at startup; it only misses *live* theme-switch IPC (irrelevant for a
  short-lived prompt).

- **A thin wrapper, `bin/omarchy-bb-prompt`, `exec`s quickshell.** The daemon appends
  `--socket <path>`; quickshell rejects unknown args, so the wrapper consumes it and forwards
  it as `BB_AUTH_SOCKET`. The wrapper `exec`s (no fork) so the recorded pid is the pid that
  connects. It launches **without `-n`/--no-duplicate** (see below).

- **The prompt exits on disconnect; the daemon relaunches a fresh, re-attestable process.**
  Provider trust is a single-use, per-launch attestation (ADR 0001). A resident provider that
  *survives* a socket drop (daemon restart, blip, config reload) and reconnects would
  re-register **untrusted** and could never become active again — and `-n` would block a
  fresh launch while the stale process is alive. So `bb-prompt.qml` calls `Qt.quit()` on a
  disconnect that follows a successful connect (`everConnected`); the daemon relaunches a
  fresh process that attests anew. Reconnect-in-place — inherited from the embedded
  `PolkitAgent` — is wrong for a daemon-launched provider. `-n` is dropped from the wrapper so
  a relaunch is never rejected as a duplicate while the old instance is still exiting.

- **The embedded plugin is disabled.** `shell/plugins/polkit/manifest.json` has `kinds: []`,
  so the omarchy shell no longer loads it as a service. The files are kept for reference;
  full removal can follow.

## Consequences

- The omarchy prompt is trusted by construction and themed for free, with zero first-prompt
  latency and self-healing across provider death and daemon restart.
- A daemon restart (or any socket drop) costs one prompt-process relaunch + re-attestation —
  the resident property holds in steady state, where the socket never drops.
- A second QML engine is resident (~tens of MB). Accepted for the latency and trust win.
- The generic provider protocol is unchanged (register/subscribe/heartbeat/session.respond);
  no first-party fast-path was added, so `PROVIDER_CONTRACT` is unchanged.

## Alternatives considered

- **Reconnect in place + multi-use trust** (daemon keeps the launch token valid while the
  launched pid lives, validating pid+start-time on each register). Avoids the relaunch but
  weakens ADR 0001's single-use anti-replay property. Rejected: exit-on-disconnect keeps the
  security model intact and is simpler.
- **Per-prompt launch** (spawn the prompt only when a session arrives). Simpler lifecycle but
  adds ~1s first-prompt latency. Rejected in favour of resident.
- **Duplicate the design system into a standalone dir** (symlink/copy `Commons`/`Ui`).
  Rejected: the config-root import path reuses it with no duplication.
- **Relax the service hardening** so quickshell can run. Unnecessary — quickshell survives the
  sandbox as-is.

## Residual limit

Same as ADR 0001: perfect isolation between same-UID processes is not achievable without
separate OS credentials. This integration raises no new trust surface — the prompt is just
another daemon-launched, SO_PEERCRED-attested provider.
