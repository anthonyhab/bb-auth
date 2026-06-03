# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

### Changed

- **BREAKING — IPC protocol v3.0.** The provider authorization model changed (see Security
  above). Free-standing third-party providers that registered under v2.x are no longer
  authorized; providers must be daemon-launched and connect directly from the launched
  process. The `pong` `version` field now reports `3.0`. See `docs/PROVIDER_CONTRACT.md`.

### Fixed

- Fallback UI no longer self-promotes to active when the daemon reports no active provider.
  Under the v3.0 fail-closed model that state means "not authorized", so the old optimistic
  self-promotion would make the first `session.respond` fail; the fallback now waits for an
  authoritative `ui.active`.

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

