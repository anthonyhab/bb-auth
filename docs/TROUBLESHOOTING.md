# Troubleshooting

## Verify runtime wiring

Run:

```bash
# For development/local install
./build-dev.sh doctor

# For packaged install, check manually:
systemctl --user status bb-auth.service
ls -l /usr/libexec/bb-auth
ls -l /usr/libexec/bb-auth-fallback
ls -l /usr/libexec/pinentry-bb
```

This checks service status, socket path, daemon binary, and gpg pinentry path.

## GPG prompt hangs on verifying

Bootstrap normally repairs stale `gpg-agent` pinentry paths automatically.

If it still fails, force-run the local fixer:

```bash
# For development/local install
./build-dev.sh fix-gpg

# For packaged install, manually configure gpg-agent:
# Edit ~/.gnupg/gpg-agent.conf and set:
# pinentry-program /usr/libexec/pinentry-bb
# Then: gpg-connect-agent reloadagent /bye
```

## Service not receiving requests

Check:

```bash
systemctl --user status bb-auth.service
journalctl --user -u bb-auth.service -n 200 --no-pager
```

If another polkit agent is running, check bootstrap logs:

```bash
journalctl --user -u bb-auth.service -n 200 --no-pager | grep bb-auth-bootstrap
```

Default policy is session-only conflict handling. Persistent disable is opt-in via service override.

## Shell UI is unavailable

Daemon should launch fallback UI automatically.

Check:

```bash
# Check legacy binary exists (packaged path)
ls -l /usr/libexec/bb-auth-fallback

# Or local install path
ls -l ~/.local/libexec/bb-auth-fallback

# Check provider manifests (drop-in providers)
ls -l ~/.config/bb-auth/providers.d
ls -l ~/.local/share/bb-auth/providers.d

# Check logs
journalctl --user -u bb-auth.service -n 200 --no-pager | grep -E "Provider launch|Discovered provider manifests|fallback"
```

If provider discovery is misconfigured, set explicit directory:

```ini
# systemctl --user edit bb-auth.service
[Service]
Environment=BB_AUTH_PROVIDER_DIR=%h/.config/bb-auth/providers.d
```

Then restart:

```bash
systemctl --user daemon-reload
systemctl --user restart bb-auth.service
```

Rollback to legacy behavior:
- Remove/disable provider manifests, or
- Set `BB_AUTH_FALLBACK_PATH` to a known-good fallback binary.
