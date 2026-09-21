# Omarchy prompt

The omarchy desktop prompt is a daemon-launched quickshell provider — themed by reusing omarchy's design system, trusted by construction via [[provider-trust#Daemon-launch attestation]], and non-resident: it exists only while a session needs it. (ADR 0002; residency made opt-out in 2026-09.)

## Resident eager launch

The daemon eager-launches the highest-priority autostart provider at startup and relaunches it on disconnect — but only when the manifest opts in (`resident: true`, the default).

- `ProviderLauncher::tryLaunch(..., eager=true)` and `CAgent::ensureFallbackUiRunning(reason, eager)` drive the lifecycle (`src/core/providers/`, `src/core/Agent.cpp`).
- Eager launch only ever selects a real autostart **and resident** provider — never the legacy env override or the built-in fallback, which stay on-demand safety nets.
- On-demand launch (`eager=false`, pending session required) still selects the same autostart manifests regardless of `resident`, so a non-resident provider keeps full priority when it matters.
- With no autostart provider configured the daemon stays fully on-demand.

## On-demand residency

A manifest may set `"resident": false` to trade first-prompt latency for footprint: nothing is launched until a session actually pends, and the provider exits when its session store drains instead of staying resident.

- The omarchy prompt uses this: the second QML engine cost (~190–210 MB resident, measured) only exists for the seconds a prompt is on screen.
- The prompt-side half lives in `BbPrompt.qml`: `sawSession` + `idleExitTimer` quit once the store drains after having served; `orphanExitTimer` bounds a launch whose session never arrives; losing active status while connected also exits (it could never serve again).
- Every session launch is a fresh process, so each prompt gets a fresh single-use attestation — non-residency is strictly stronger than residency under [[provider-trust#Daemon-launch attestation]], not weaker.
- Cost: ~0.5–1.5 s of quickshell cold-start latency per prompt (ADR 0002 chose resident for latency; reversed when the resident footprint was measured).

## Design system reuse

The entry `shell/bb-prompt.qml` runs as `quickshell -p <shell-dir>/bb-prompt.qml`; the config-root import path resolves `qs.Commons`/`qs.Ui` so nothing is duplicated.

- There is **no `-I` flag** in quickshell — config root is the directory holding the launched file.
- `Style`/`Color` singletons self-initialise from `~/.config/omarchy/current/theme/*.toml`, so the standalone process is themed at startup (it only misses live theme-switch IPC, irrelevant for a short-lived prompt).
- The embedded polkit plugin (`shell/plugins/polkit/`) is disabled via `kinds: []` in its manifest; files kept for reference.

## Exit on disconnect

The prompt calls `Qt.quit()` when a disconnect follows a successful connect (`everConnected`), and the daemon relaunches a fresh process that attests anew.

- Trust is single-use per launch ([[provider-trust#Daemon-launch attestation]]): a provider that *survived* a socket drop would re-register untrusted and could never become active again.
- The thin wrapper `bin/omarchy-bb-prompt` `exec`s quickshell (no fork) so the recorded pid is the pid that connects; it consumes the daemon's `--socket` arg and forwards it as `BB_AUTH_SOCKET`.
- The wrapper launches **without `-n`/`--no-duplicate`** so a relaunch is never rejected while the old instance is still exiting.
- Reconnect-in-place — inherited from the old embedded `PolkitAgent` — is wrong for a daemon-launched provider.

## Sandbox survival

A `startDetached` child inherits the daemon's hardened systemd unit (MDWE, `@system-service` filter, `ProtectSystem=strict`, empty capability set) and quickshell provably runs under it.

- The V4 engine falls back to the interpreter under `MemoryDenyWriteExecute`; the layer-shell window still renders. **No hardening relaxation is needed.**
- Non-resident launch keeps this property: the on-demand `startDetached` path is the same code path as eager launch.
