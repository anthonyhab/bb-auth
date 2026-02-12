# bb-auth

`bb-auth` is a Linux desktop authentication daemon that replaces fragmented auth prompts with one service.

It handles:
- polkit actions (`pkexec`, privileged app operations)
- GNOME Keyring system prompts
- GPG pinentry prompts

It is designed to work with a shell UI provider (`bb-auth` shell plugin). If no provider is active, it launches a built-in fallback window (`bb-auth-fallback`) so prompts still appear.

## Contents

- [Quick start (most users)](#quick-start-most-users)
- [Expected runtime behavior](#expected-runtime-behavior)
- [Wayland and X11 behavior](#wayland-and-x11-behavior)
- [Conflict policy (`BB_AUTH_CONFLICT_MODE`)](#conflict-policy-bb_auth_conflict_mode)
- [Optional compositor hints for fallback window](#optional-compositor-hints-for-fallback-window)
- [Troubleshooting and FAQ](#troubleshooting-and-faq)
- [Migration from `noctalia-auth`](#migration-from-noctalia-auth)
- [Development workflow](#development-workflow)
- [Screenshots](#screenshots)

## Quick start (most users)

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

**Build from source**

Dependencies (names vary by distro): Qt6 base, polkit-qt6, polkit, gcr-4, json-glib, cmake, pkg-config.

```bash
git clone https://github.com/anthonyhab/bb-auth
cd bb-auth
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

### 2) Enable the user service

`bb-auth` runs as a **systemd user service**.

```bash
systemctl --user daemon-reload
systemctl --user enable --now bb-auth.service
```

If `systemctl --user ...` fails because your user manager/session is not active yet, log out and back in, then run the commands again.

### 3) Smoke check

```bash
pkexec true
echo test | secret-tool store --label=test attr val
```

Optional service check:

```bash
systemctl --user status bb-auth.service
```

If prompts do not appear, start with:

```bash
./build-dev.sh doctor
```

Then use the troubleshooting guide: `docs/TROUBLESHOOTING.md`.

## Expected runtime behavior

- Auth requests (polkit/keyring/pinentry) are routed through `bb-auth`.
- If no shell provider is active when a prompt is needed, `bb-auth-fallback` is launched automatically.
- The fallback enforces single-instance behavior and exits when a higher-priority shell provider becomes active.

Runtime contract (identifiers and paths):
- Service: `bb-auth.service`
- Socket: `$XDG_RUNTIME_DIR/bb-auth.sock`
- D-Bus names:
  - `org.bb.auth`
  - `org.gnome.keyring.SystemPrompter`
- Main binary: `bb-auth`
- Pinentry binary: `pinentry-bb` (symlink to `bb-auth`)
- Fallback binary: `bb-auth-fallback`

On service start, bootstrap runs automatically and:
- validates/repairs `gpg-agent` pinentry path
- applies conflict policy for known competing polkit agents

## Wayland and X11 behavior

By default, `bb-auth.service` is scoped to Wayland sessions.

The unit template includes:
- `ConditionEnvironment=WAYLAND_DISPLAY`
- source: `assets/bb-auth.service.in:5`

This prevents the service from auto-starting in non-Wayland sessions unless you override/remove that condition.

## Conflict policy (`BB_AUTH_CONFLICT_MODE`)

Default mode is `session`.

| Mode | Behavior |
|---|---|
| `session` (default) | Stops known competing agents/services for the current login session only. |
| `persistent` | Also disables known competing user services and writes autostart overrides. Existing user autostarts are backed up under `~/.local/state/bb-auth/autostart-backups/`. |
| `warn` | Detect-only behavior; no stopping/disabling. |

Set mode with a user-service override:

```bash
systemctl --user edit bb-auth.service
```

```ini
[Service]
Environment=BB_AUTH_CONFLICT_MODE=session
```

## Optional compositor hints for fallback window

The fallback app is a normal top-level window so compositors can manage it.

**Hyprland (0.53+)**
```ini
windowrule = float, class:^(bb-auth-fallback)$
windowrule = center, class:^(bb-auth-fallback)$
windowrule = size 560 360, class:^(bb-auth-fallback)$
```

**Niri**
```kdl
window-rule {
    match app-id="bb-auth-fallback"
    default-column-width { fixed 560; }
    default-window-height { fixed 360; }
    open-floating true
    center-on-cursor true
}
```

**Sway / i3**
```
for_window [app_id="bb-auth-fallback"] floating enable, resize set 560 360, move position center
```

## Troubleshooting and FAQ

### If prompts do not show

1. Run `./build-dev.sh doctor` (dev/local installs).
2. Check service state: `systemctl --user status bb-auth.service`.
3. Read `docs/TROUBLESHOOTING.md` for focused diagnosis paths.

### FAQ

**Why does the service not start on X11?**  
Because the user unit is Wayland-gated by default via `ConditionEnvironment=WAYLAND_DISPLAY` (`assets/bb-auth.service.in:5`). Remove/override that line if you intentionally want X11 behavior.

**I already have other polkit agents. What does bb-auth do?**  
It applies the configured conflict mode at startup. Default `session` mode only affects the current session; `persistent` also disables known competing user services/autostarts; `warn` only detects.

**Where is the socket, and how do providers connect?**  
The runtime socket is `$XDG_RUNTIME_DIR/bb-auth.sock`. Shell providers connect to the daemon through its runtime interfaces; when no provider is active, fallback UI is auto-launched.

**How do I reset/fix pinentry configuration?**  
For dev/local installs: run `./build-dev.sh fix-gpg`. For packaged installs: set `pinentry-program /usr/libexec/pinentry-bb` in `~/.gnupg/gpg-agent.conf`, then reload agent (`gpg-connect-agent reloadagent /bye`).

## Migration from `noctalia-auth`

This project was previously named `noctalia-auth` (daemon) and `polkit-auth` (plugin).

After installing `bb-auth`, run:

```bash
bb-auth-migrate
# Or remove legacy binaries automatically:
bb-auth-migrate --remove-binaries
```

Migration script behavior:
- stops/disables old `noctalia-auth.service`
- cleans legacy runtime files/state
- backs up old state directory
- reports legacy binaries for cleanup
- enables/starts `bb-auth.service` automatically (unless `--no-enable`)

Skip auto-enable if needed:

```bash
bb-auth-migrate --no-enable
```

Plugin ID changed from `polkit-auth` to `bb-auth`; reinstall it through your shell plugin UI.

## Development workflow

```bash
./build-dev.sh install
./build-dev.sh enable
./build-dev.sh doctor
```

Useful commands:

```bash
./build-dev.sh status
./build-dev.sh disable
./build-dev.sh uninstall
```

## Screenshots

### Fallback prompt window (`bb-auth-fallback`)

![Fallback prompt window (bb-auth-fallback)](assets/screenshot.png)

Shown when no shell provider is active, so authentication prompts remain available.

### Planned additional screenshots

- Shell provider prompt
- Keyring prompt flow
- Pinentry prompt flow
