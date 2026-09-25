# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.0] - 2026-09-25

The agent-supervision release: when an AI coding agent runs `sudo`, the
escalation becomes a handoff to you — attributed, explained, and approved at
the bb-auth prompt — and one command wires it into every supported harness.

### Added

- **Agent attribution at the prompt.** Requests from AI agents show an "AI
  agent" badge, the OS-resolved requestor, and the agent's declared reason (or
  an explicit "No reason declared by the agent."). A declared agent that
  disagrees with the resolved one shows a mismatch warning. Attribution is
  display/audit only and never affects allow/deny (ADR 0003).
- **`intent.declare` IPC** binding a self-asserted reason to the declarer's
  OS-resolved agent ancestry (pid + start-time; TTL'd, consume-once).
- **Harness hook `bb-auth-intent-hook`** for Claude Code, Codex CLI, Devin, and
  Gemini CLI, plus an opencode plugin and a pi extension. A clean leading
  `sudo CMD`/`doas CMD` is rewritten to `pkexec CMD` so it reaches the
  supervised prompt; the agent's last message becomes the reason where the
  harness exposes a transcript.
- **Challenge-gated handoff.** The rewrite (and the harness-level "allow" it
  carries) is emitted only when `pkcheck` reports polkit will prompt you; the
  model is told a human owns the decision and that exit 126/127 means no.
  Claude Code post-run events annotate results for the auto mode classifier.
- **`bb-auth-agents status|install|uninstall`** — idempotent, backed-up wiring
  of every detected harness, including `bb-auth:` entries in Claude Code's
  `autoMode` (with `$defaults` preserved).
- **`aisudo`** voluntary CLI and agent-gated `sudo`/`doas`/`pkexec` PATH shims
  for hookless harnesses; one shared translation subset (`-n`, `-u`, `--`).
- **One compiled binary, no Python.** `bb-auth-agent` (Qt6::Core) is the hook,
  `aisudo`, the shims, and `bb-auth-agents`, selected by `argv[0]`; agent
  detection shares the daemon's code. Config edits preserve key order and
  formatting.
- **On-demand providers** (`"resident": false`): launched per session, exit
  when idle. The omarchy prompt uses it.
- Forgejo CI and tag-driven release pipeline; stable AUR `PKGBUILD` pinned to
  the verified commit.

### Changed

- **BREAKING — provider IPC 3.0.** Only a provider the daemon launched from a
  `providers.d` manifest can become active (see Security); free-standing v2.x
  providers are no longer authorized. `pong.version` reports `3.0`. Migration:
  `docs/COMPATIBILITY.md`.
- Orphaned sessions expire after a TTL instead of lingering.

### Security

Provider authorization hardening against a hostile same-UID process (the threat model for
an auth agent that supervises other agents). See `docs/adr/0001-provider-trust-model.md`.

- **Provider trust by daemon-launch attestation (F1/F2).** Only a provider the daemon
  launched — proven by `SO_PEERCRED` pid + start-time — can become active and receive the
  user's secret. `priority` is clamped to `[-1000, 1000]`. The legacy "no providers
  registered → any socket authorized" mode is removed; authorization now fails closed.
- **Pinentry result owner check fails closed (F3).** A result for a cookie with no recorded
  owner is rejected instead of accepted.
- **Restricted session-event delivery (F4).** Session events (prompt text, requestor
  identity) reach only the active (attested) provider — never other subscribers or `next`
  pull-waiters, and `subscribe` replay is active-provider-only.
- **Keyring session owner binding (F5).** A `keyring_request` whose cookie is already held
  by a different peer is rejected before any state mutation, closing a cookie-collision
  strand.
- **DoS hardening (F6/F7).** Provider manifests over 64 KiB are skipped before being read;
  a socket that buffers an incomplete frame is disconnected after a timeout; caller-supplied
  keyring `flags` are bounded.

### Fixed

- Hook rewrites keep the harness's other tool-input fields (`timeout`,
  `run_in_background`) instead of dropping them.
- A second daemon can no longer hijack a running agent's socket path.

- Fallback UI no longer self-promotes to active when the daemon reports no active provider.
  Under the v3.0 fail-closed model that state means "not authorized", so the old optimistic
  self-promotion would make the first `session.respond` fail; the fallback now waits for an
  authoritative `ui.active`.

### Removed

- `bb-auth-declare` alias (use `aisudo`).
- GitHub Actions workflows — Forgejo is the CI authority; GitHub is a mirror.

## [0.2.0] - 2026-02-18

### Changed

- Qt-first modular core; the in-tree GTK provider moved out. External
  providers register through `providers.d` manifests; provider IPC locked at
  v2.0 with a conformance suite and an external provider template.

### Security

- Fail closed on polkit session-id collisions and session overwrite.
- Bridge detection (`pkexec`/`sudo`/`doas`) uses the executable, not the
  self-settable process name.
- Single-instance check uses `QLockFile` instead of `pgrep`.

## [0.1.0] - 2025-02-15

### Added
- Initial public release
- Unified polkit, keyring, and pinentry authentication daemon
- Shell provider IPC protocol for Waybar, ags, and custom widgets
- Fallback window with touch sensor support (fingerprint, FIDO2)
- Systemd user service with security hardening
- D-Bus service integration
- Bootstrap script for automatic configuration
- Migration script (`bb-auth-migrate`) for users upgrading from noctalia-auth
- Comprehensive documentation (README, troubleshooting, provider contract)
- Test suite with QtTest coverage
- AUR PKGBUILD with check() function
- Nix flake support
- CI workflows for Arch Linux and Nix builds

