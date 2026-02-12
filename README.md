# bb-auth

`bb-auth` is a Linux desktop authentication daemon for polkit, GNOME Keyring system prompts, and GPG pinentry.

It expects a `bb-auth` shell provider. If no provider is active, it auto-launches `bb-auth-fallback` so prompts still appear.

## Quick start

### 1) Install

**AUR**
```bash
yay -S bb-auth-git
```

**Nix**
```bash
nix build .#bb-auth
nix profile install .#bb-auth
```

**Dev/local build**
```bash
./build-dev.sh install
```

### 2) Enable user service

```bash
systemctl --user daemon-reload
systemctl --user enable --now bb-auth.service
```

### 3) Smoke test

```bash
pkexec true
echo test | secret-tool store --label=test attr val
./build-dev.sh doctor
```

If prompts do not show, start with [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Runtime behavior

- Requests route through `bb-auth`.
- If no shell provider is active, `bb-auth-fallback` is launched automatically.
- Fallback is single-instance and exits when a higher-priority provider appears.

Runtime contract:
- Service: `bb-auth.service`
- Socket: `$XDG_RUNTIME_DIR/bb-auth.sock`
- D-Bus names: `org.bb.auth`, `org.gnome.keyring.SystemPrompter`

## Wayland / X11

The shipped user service is Wayland-gated with `ConditionEnvironment=WAYLAND_DISPLAY` (see `assets/bb-auth.service.in:5`). On X11, remove or override that condition in a user unit override.

## Conflict policy (`BB_AUTH_CONFLICT_MODE`)

| Mode | Behavior |
|---|---|
| `session` (default) | Stop known competing agents/services for the current session only. |
| `persistent` | Also disable competing user services and write autostart overrides (backups in `~/.local/state/bb-auth/autostart-backups/`). |
| `warn` | Detect conflicts only; no stopping/disable actions. |

Set the mode with a user override:

```bash
systemctl --user edit bb-auth.service
```

```ini
[Service]
Environment=BB_AUTH_CONFLICT_MODE=session
```

## Troubleshooting + FAQ

- Troubleshooting guide: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)

**No prompts appear**  
Run `./build-dev.sh doctor`, then follow the troubleshooting guide.

**Why doesn't it start on X11?**  
The default service has a Wayland gate (`assets/bb-auth.service.in:5`).

**What if another polkit agent is already running?**  
Behavior is controlled by `BB_AUTH_CONFLICT_MODE`.

## Migration from `noctalia-auth`

```bash
bb-auth-migrate
# Optional cleanup:
bb-auth-migrate --remove-binaries
# Skip auto-enable/start:
bb-auth-migrate --no-enable
```

## Screenshot

![Fallback prompt window (bb-auth-fallback)](assets/screenshot.png)
