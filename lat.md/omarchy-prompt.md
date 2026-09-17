# Omarchy prompt

The omarchy desktop prompt is a resident, daemon-launched quickshell provider — themed by reusing omarchy's design system, trusted by construction via [[provider-trust#Daemon-launch attestation]]. (ADR 0002.)

## Resident eager launch

After the IPC server binds, the daemon eager-launches the highest-priority **autostart** provider and relaunches it on disconnect, keeping a prompt resident for zero first-prompt latency.

- `ProviderLauncher::tryLaunch(..., eager=true)` and `CAgent::ensureFallbackUiRunning(reason, eager)` drive the lifecycle (`src/core/providers/`, `src/core/Agent.cpp`).
- Eager launch only ever selects a real autostart provider — never the legacy env override or the built-in fallback, which stay on-demand safety nets.
- With no autostart provider configured the daemon stays fully on-demand.

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
- Cost: a second resident QML engine (~tens of MB), accepted for the latency and trust win.
