# External Provider Template

This template is a minimal external `bb-auth` provider implemented in Python.

It demonstrates:

- connecting to daemon socket
- sending `ui.register`
- sending `subscribe`
- sending periodic `ui.heartbeat`

Use this as a starting point for providers in any language.

## Files

- `provider.py`: minimal provider executable
- `provider.json`: manifest template

## Quick local test

1. Build/install `bb-auth` core.
2. Copy template artifacts:

```bash
mkdir -p ~/.local/libexec ~/.local/share/bb-auth/providers.d
cp examples/provider-template/provider.py ~/.local/libexec/bb-auth-provider-template
chmod +x ~/.local/libexec/bb-auth-provider-template
cp examples/provider-template/provider.json ~/.local/share/bb-auth/providers.d/provider-template.json
```

3. Edit `exec` in `provider-template.json` to your absolute path or basename in `PATH`.
4. Restart service:

```bash
systemctl --user restart bb-auth.service
```

5. Trigger an auth flow (`pkexec`, keyring, or pinentry) and inspect logs:

```bash
journalctl --user -u bb-auth.service -n 100 --no-pager
```
