# Provider Packaging Guide

This document describes how to ship external `bb-auth` providers as standalone packages.

## Goals

- Keep `bb-auth` core package minimal.
- Ship provider dependencies separately.
- Let users install/remove providers without rebuilding core.

## Required Artifacts

Each provider package should install:

1. Provider executable (any language/runtime).
2. Provider manifest (`*.json`) under a `providers.d` directory.

Starter template:

- `examples/provider-template/`

## Runtime Locations

`bb-auth` discovers manifests in this order:

1. `BB_AUTH_PROVIDER_DIR`
2. `${XDG_CONFIG_HOME:-~/.config}/bb-auth/providers.d`
3. `${XDG_DATA_HOME:-~/.local/share}/bb-auth/providers.d`
4. System data dir (packaged install)

For distro packages, install manifest to:

- `/usr/share/bb-auth/providers.d/`

Install provider binary to:

- `/usr/libexec/` (recommended), or
- another stable path referenced by absolute `exec`.

## Manifest Template

```json
{
  "id": "my-provider",
  "name": "My Provider",
  "kind": "custom",
  "priority": 20,
  "exec": "my-provider-binary",
  "autostart": true,
  "resident": true,
  "capabilities": ["password", "cancel", "status"]
}
```

Notes:

- `id` must be unique across loaded manifests.
- `exec` can be basename (resolved in `PATH`) or absolute path.
- Higher `priority` wins active-provider selection.
- `autostart` makes the provider eligible for launch at all; without it the built-in fallback serves sessions instead.
- `resident` (default `true`) controls whether the daemon keeps the provider hot via eager launches (daemon startup, post-disconnect). Set `false` to launch only when a session pends — the provider should then exit when its session store drains, since nothing keeps it alive. Each launch is a fresh attestation, so non-resident providers are strictly stronger under the trust model at the cost of launch latency per prompt.

## Arch Packaging Example

Package structure example:

- `/usr/libexec/bb-auth-my-provider`
- `/usr/share/bb-auth/providers.d/my-provider.json`

`PKGBUILD` guidance:

- `depends` should include only what your provider needs.
- add `depends=('bb-auth')` so core is installed.
- do not modify core package files outside your own artifacts.

## Nix Packaging Example

- Build provider derivation independently.
- Add `bb-auth` as runtime dependency.
- Install manifest into `$out/share/bb-auth/providers.d`.
- Install binary into `$out/libexec` and use absolute `exec` in manifest if not wrapped into global `PATH`.

## Verification Checklist

After install:

```bash
systemctl --user restart bb-auth.service
journalctl --user -u bb-auth.service -n 50 --no-pager
```

Validate provider was discovered and selected:

- look for discovery logs and launch status
- trigger auth prompt (`pkexec`, keyring, pinentry flow)

## Compatibility

- Provider IPC behavior must match `docs/PROVIDER_CONTRACT.md`.
- Breaking protocol changes require explicit versioning and migration notes.
