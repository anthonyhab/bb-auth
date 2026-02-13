# bb-auth

Linux desktop authentication that stays out of your way.

![Fallback prompt](assets/screenshot.png)

**bb-auth** handles polkit elevation, GNOME Keyring unlocks, and GPG pinentry with a unified prompt system. It works with your shell (Waybar, ags, etc.) or shows a standalone fallback window when needed.

---

## What it does

| Without bb-auth | With bb-auth |
|-----------------|--------------|
| polkit-gnome popup windows | Prompts in your shell bar |
| `secret-tool` hangs silently | Prompts appear, then auto-unlock |
| GPG prompts in terminal | GUI prompts with touch sensor support |
| Multiple inconsistent UIs | One system, your styling |

**The key idea:** Your shell provides the UI. If it can't, a lightweight fallback window appears automatically. Nothing blocks, nothing hangs.

---

## Requirements

- Linux with **Wayland** (X11 works with config override)
- `polkit` daemon running
- One of: AUR helper, Nix, or manual build tools

---

## Install

Pick one:

**Arch Linux (AUR)** — recommended for most users
```bash
yay -S bb-auth-git
```

**Nix**
```bash
nix profile install github:anthonyhab/bb-auth#bb-auth
```

**Manual build**
```bash
git clone https://github.com/anthonyhab/bb-auth
cd bb-auth
./build-dev.sh install
```

---

## First run

1. **Enable the service**
   ```bash
   systemctl --user daemon-reload
   systemctl --user enable --now bb-auth.service
   ```

2. **Verify it's running**
   ```bash
   systemctl --user status bb-auth.service
   # Should show "active (running)"
   ```

3. **Test a prompt**
   ```bash
   pkexec echo "it works"
   ```

   You should see a fallback window (we haven't set up shell integration yet).

---

## Add to your shell (optional but recommended)

The fallback works fine. A shell provider looks better.

**What's a shell provider?** A small widget that connects to bb-auth and draws prompts in your bar/panel.

| Shell | Provider | Install |
|-------|----------|---------|
| ags (Aylur's GTK Shell) | bb-ags (coming soon) | `yay -S bb-ags` |
| Waybar | bb-waybar (coming soon) | `yay -S bb-waybar` |
| Custom | Protocol docs below | See `docs/PROTOCOL.md` |

Once a provider connects, fallback auto-exits. If the provider crashes, fallback auto-starts. You don't manage this.

---

## Daily use

**Elevation:** `pkexec command` — prompt appears in shell, or fallback window  
**Keyring:** `secret-tool store --label="Email" service gmail` — unlocks once, stays unlocked  
**GPG:** `git commit -S` — pinentry with fingerprint reader support

**First boot note:** On a fresh login, the first keyring unlock may prompt twice (once for login keyring, once for the app). This is normal GNOME Keyring behavior, not bb-auth.

---

## Common issues

**"pkexec hangs with no prompt"**
```bash
# Check service is running
systemctl --user status bb-auth.service

# Check logs
journalctl --user -u bb-auth.service -n 50

# Likely cause: another polkit agent is running
killall polkit-gnome-authentication-agent-1
systemctl --user restart bb-auth.service
```

**"GPG prompts still go to terminal"**
```bash
# Check pinentry path
ls -l /usr/libexec/pinentry-bb

# If missing or wrong, run bootstrap
bb-auth-bootstrap

# Or manually edit ~/.gnupg/gpg-agent.conf:
pinentry-program /usr/libexec/pinentry-bb
gpg-connect-agent reloadagent /bye
```

**"Service fails on X11"**
The default service has a Wayland gate. Edit the override:
```bash
systemctl --user edit bb-auth.service
```
Remove or comment out:
```ini
# ConditionEnvironment=WAYLAND_DISPLAY
```

**"Prompts look wrong / touch sensor not working"**
The fallback UI includes touch sensor support (fingerprint, FIDO2). If detection fails:
```bash
# Force password mode
BB_AUTH_FALLBACK_FORCE_PASSWORD=1 pkexec command
```

See [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) for deeper debugging.

---

## How it works

```
┌─────────────────┐     ┌──────────────┐     ┌──────────────────┐
│   Application   │────▶│   bb-auth    │────▶│  Shell provider  │
│  (pkexec/gpg)   │     │   daemon     │     │  (Waybar/ags)    │
└─────────────────┘     └──────────────┘     └──────────────────┘
                               │
                               └────▶ Fallback window (if no shell)
```

**Priority system:** Multiple providers can connect. Highest priority wins. Tie breaks by most recent heartbeat. Dead providers auto-prune.

**Conflict handling:** If another polkit agent runs, bb-auth can stop it (default: session-only), warn only, or do nothing. Configurable via `BB_AUTH_CONFLICT_MODE`.

---

## Configuration

**Environment variables** (set in service override):

| Variable | Values | Default |
|----------|--------|---------|
| `BB_AUTH_CONFLICT_MODE` | `session`, `persistent`, `warn` | `session` |
| `BB_AUTH_FALLBACK_PATH` | Path to binary | auto-detected |

**Service override:**
```bash
systemctl --user edit bb-auth.service
```

```ini
[Service]
Environment=BB_AUTH_CONFLICT_MODE=warn
```

---

## Migration from noctalia-auth

```bash
bb-auth-migrate

# Optional: remove old binaries
bb-auth-migrate --remove-binaries
```

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

---

## License

MIT
