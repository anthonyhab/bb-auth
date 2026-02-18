# bb-auth

Linux desktop authentication that stays out of your way.

![Fallback prompt](assets/screenshot.png)

**bb-auth** handles polkit elevation, GNOME Keyring unlocks, and GPG pinentry with a unified prompt system. It shows a lightweight fallback window when your shell can't provide the UI.

---

## What it does

| Without bb-auth | With bb-auth |
|-----------------|--------------|
| polkit-gnome popup windows | Clean prompt window (or your shell bar when providers arrive) |
| `secret-tool` hangs silently | Prompts appear, then auto-unlock |
| GPG prompts in terminal | GUI prompts with touch sensor support |
| Multiple inconsistent UIs | One system, your styling |

**The key idea:** A lightweight fallback window appears automatically when needed. Nothing blocks, nothing hangs.

---

## Requirements

- Linux with **Wayland** (X11 works with config override)
- `polkit` daemon running
- One of: AUR helper, Nix, or manual build tools

---

## Quickstart

### Install

**Arch Linux (AUR)** — recommended for most users:
```bash
yay -S bb-auth-git
```

`bb-auth-git` defaults to `-DBB_AUTH_GTK_FALLBACK=AUTO`: GTK fallback is built only when `gtk4` is available in the build environment.

**Nix**:
```bash
nix profile install github:anthonyhab/bb-auth#bb-auth
```

**Manual build**:
```bash
git clone https://github.com/anthonyhab/bb-auth
cd bb-auth
./build-dev.sh install
```

### Enable the service

bb-auth runs as a **user service** (no sudo needed):

```bash
systemctl --user daemon-reload
systemctl --user enable --now bb-auth.service
```

Verify it's running:
```bash
systemctl --user status bb-auth.service
journalctl --user -u bb-auth.service -n 20 --no-pager
```

You should see "active (running)" in the status output.

### Test it

```bash
pkexec echo "it works"
```

You should see a fallback prompt window. Enter your password — the command should complete successfully.

Other tests:
```bash
# Keyring unlock
secret-tool store --label="Test" service test-app

# GPG signing (if you have a key configured)
git commit -S -m "test" --allow-empty
```

---

## Troubleshooting

### "pkexec hangs with no prompt"

```bash
# Check service status
systemctl --user status bb-auth.service

# View recent logs
journalctl --user -u bb-auth.service -n 50 --no-pager

# Likely cause: another polkit agent is running
killall polkit-gnome-authentication-agent-1
systemctl --user restart bb-auth.service
```

### "GPG prompts still go to terminal"

First, restart the service (this runs bootstrap automatically):
```bash
systemctl --user restart bb-auth.service
```

If that doesn't work, check the pinentry path and manually configure:
```bash
# Check if the pinentry wrapper exists
ls -l /usr/libexec/pinentry-bb

# Manually edit gpg-agent config
# Add to ~/.gnupg/gpg-agent.conf:
# pinentry-program /usr/libexec/pinentry-bb
# Then reload:
gpg-connect-agent reloadagent /bye
```

For development/local installs, the path is `~/.local/libexec/pinentry-bb`.

### "Service fails on X11"

The default service has a Wayland gate. Edit the override:
```bash
systemctl --user edit bb-auth.service
```

Remove or comment out:
```ini
# ConditionEnvironment=WAYLAND_DISPLAY
```

Then restart the service.

### "Prompts look wrong / touch sensor not working"

The fallback UI includes touch sensor support (fingerprint, FIDO2). If detection fails:
```bash
# Force password mode
BB_AUTH_FALLBACK_FORCE_PASSWORD=1 pkexec command
```

See [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) for deeper debugging.

---

## Advanced

### Environment variables

Set these via service override (`systemctl --user edit bb-auth.service`):

| Variable | Values | Default | Description |
|----------|--------|---------|-------------|
| `BB_AUTH_CONFLICT_MODE` | `session`, `persistent`, `warn` | `session` | How to handle other polkit agents. `session` = stop them for this session only. `persistent` = disable them permanently. `warn` = just log a warning. |
| `BB_AUTH_FALLBACK_PATH` | Path to binary | auto-detected | Highest-precedence legacy fallback override. If set, daemon launches this binary directly. |
| `BB_AUTH_PROVIDER_DIR` | Directory path | unset | Optional highest-precedence providers manifest directory (`*.json`). |

Example override:
```ini
[Service]
Environment=BB_AUTH_CONFLICT_MODE=warn
Environment=BB_AUTH_PROVIDER_DIR=%h/.config/bb-auth/providers.d
```

### GTK fallback build mode

Use CMake option `BB_AUTH_GTK_FALLBACK`:

- `AUTO` (default): build GTK fallback only if `gtk4` is found.
- `ON`: require and build GTK fallback (configure fails if missing).
- `OFF`: never build GTK fallback.

AUR builds can override with:
```bash
BB_AUTH_GTK_FALLBACK=OFF makepkg -si
```

### Drop-in providers (`providers.d`)

Provider executables are discovered through manifest files and launched automatically when needed.

Search order (first wins by manifest `id`):

1. `BB_AUTH_PROVIDER_DIR`
2. `${XDG_CONFIG_HOME:-~/.config}/bb-auth/providers.d`
3. `${XDG_DATA_HOME:-~/.local/share}/bb-auth/providers.d`
4. `${CMAKE_INSTALL_DATADIR}/bb-auth/providers.d` (packaged install)

Manifest requirements:

```json
{
  "id": "gtk-fallback",
  "name": "GTK Fallback Provider",
  "kind": "gtk-fallback",
  "priority": 8,
  "exec": "bb-auth-gtk-fallback",
  "autostart": true
}
```

Validation rules:
- `id`: `[a-z0-9][a-z0-9._-]*`
- `priority`: integer in `[-1000, 1000]`
- `exec`: absolute path or basename resolvable in `PATH`

If manifests are invalid or missing, daemon falls back to the existing Qt fallback path. This rollout is additive and backward-compatible.

### Migration from noctalia-auth

If you're upgrading from the old `noctalia-auth` package:

```bash
/usr/libexec/bb-auth-migrate

# Optional: remove old binaries
/usr/libexec/bb-auth-migrate --remove-binaries
```

For local installs: `~/.local/libexec/bb-auth-migrate`

---

## Development

```bash
# Build and run tests
./build-dev.sh test

# Run with verbose logging
./build-dev.sh run --verbose

# Check wiring
./build-dev.sh doctor
```

See [docs/PINENTRY_FLOW.md](docs/PINENTRY_FLOW.md) for pinentry flow details and [docs/PROVIDER_CONTRACT.md](docs/PROVIDER_CONTRACT.md) for provider IPC contract.

---

## License

BSD-3-Clause — see [LICENSE](LICENSE)
