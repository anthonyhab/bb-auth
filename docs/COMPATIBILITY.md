# Compatibility matrix

Core release ↔ provider IPC protocol. The normative contract is
`docs/PROVIDER_CONTRACT.md`; this page is the operator-facing summary.

## Core ↔ provider IPC

| bb-auth release | `pong.version` | Contract | Provider requirements |
|---|---|---|---|
| v0.1.x | none (pre-contract) | — | No external provider support; built-in fallback only |
| v0.2.0 | `2.0` | IPC v2.0 | Providers self-register over the socket; any registered provider can become active |
| next release (0.3.0, unreleased) | `3.0` | IPC v3.0 | Providers must be daemon-launched from a `providers.d` manifest to become active; agent-attribution fields added |

Notes:

- IPC `2.1` (additive agent-attribution fields) exists only in the contract
  document — no released build ever advertised it; the wire version went
  `2.0` → `3.0` in the same change.
- `pong.version` is the authoritative check: `bb-auth --ping` (or any client
  `ping`) returns it.

## Upgrade notes

### v0.2.0 → IPC 3.0 builds

Breaking change — authorization model:

- A provider the daemon did **not** launch can still connect and register, but
  can never become active and is never authorized to submit
  `session.respond`/`session.cancel`.
- Migration for v2.x free-standing providers: ship a `providers.d/*.json`
  manifest and let the daemon spawn the process. See `docs/PROVIDER_PACKAGING.md`
  and `examples/provider-template/`.
- Additive fields a v2.0 parser must ignore per contract §3:
  `context.requestor.isAgent`, `context.requestor.agentKind`, `context.intent`,
  and the `intent.declare` provider→daemon message.

Upgrade checks after installing:

```bash
bb-auth --ping            # expect "version":"3.0"
systemctl --user status bb-auth
# verify each provider manifest parses and launches:
journalctl --user -u bb-auth -b | grep -i provider
```

### v0.1.x → v0.2.0

- Provider contract introduced and locked at IPC v2.0. Free-standing external
  providers connect to the daemon socket and self-register.
- The GTK provider moved out of core: install it as a separate package with its
  own manifest.

## Release checklist

Each release MUST ship with:

- [ ] `VERSION` bumped; `pong.version` matches the advertised contract version.
- [ ] `docs/PROVIDER_CONTRACT.md` status line updated for any protocol change.
- [ ] Migration notes added here for every major/minor protocol bump.
- [ ] `./scripts/gate-local.sh` green on the release commit.
- [ ] CI green: `arch.yml` (core-only + core-plus-provider) and `nix.yml`.
