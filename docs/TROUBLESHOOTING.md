# Troubleshooting

## Verify runtime wiring

Run:

```bash
./build-dev.sh doctor
```

This checks service status, socket path, daemon binary, and gpg pinentry path.

## GPG prompt hangs on verifying

Bootstrap normally repairs stale `gpg-agent` pinentry paths automatically.

If it still fails, force-run the local fixer:

```bash
./build-dev.sh fix-gpg
```

## Service not receiving requests

Check:

```bash
systemctl --user status noctalia-auth.service
journalctl --user -u noctalia-auth.service -n 200 --no-pager
```

If another polkit agent is running, check bootstrap logs:

```bash
journalctl --user -u noctalia-auth.service -n 200 --no-pager | grep noctalia-auth-bootstrap
```

Default policy is session-only conflict handling. Persistent disable is opt-in via service override.

## Shell UI is unavailable

Daemon should launch fallback UI automatically.

Check:

```bash
ls -l ~/.local/libexec/noctalia-auth-fallback
journalctl --user -u noctalia-auth.service -n 200 --no-pager | grep "Launched fallback UI"
```
