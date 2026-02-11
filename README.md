# noctalia-auth

Unified authentication daemon for Noctalia Shell.

- Polkit authentication agent
- GNOME Keyring system prompter replacement
- GPG pinentry bridge

This daemon is consumed by the `polkit-auth` plugin in `noctalia-plugins`.

## Runtime Contract

- Service: `noctalia-auth.service`
- Socket: `$XDG_RUNTIME_DIR/noctalia-auth.sock`
- Main binary: `noctalia-auth`
- Pinentry binary: `pinentry-noctalia`
- Fallback UI binary: `noctalia-auth-fallback`

## Install

### AUR

```bash
yay -S noctalia-auth-git
```

### Manual build

Dependencies (distro names vary): Qt6 base, polkit-qt6, polkit, gcr-4, json-glib, cmake, pkg-config.

```bash
git clone https://github.com/anthonyhab/noctalia-polkit
cd noctalia-polkit
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

Enable the service:

```bash
systemctl --user daemon-reload
systemctl --user enable --now noctalia-auth.service
```

On each service start, bootstrap automatically:

- validates and repairs `gpg-agent` pinentry path when stale
- stops known competing polkit agents for the current session

If no shell UI provider is active when an auth request arrives, daemon launches `noctalia-auth-fallback` automatically.
The fallback app enforces a single-instance lock and stands down when a higher-priority shell provider becomes active.

For tiling compositors, the fallback app uses a normal top-level window so it can still be tiled manually by the compositor.
On Hyprland, add a class-based rule if you want default floating + centering on open:

```ini
windowrule {
  name = Noctalia Auth Fallback
  float = on
  center = on
  size = 560 360
  match:class = noctalia-auth-fallback
}
```

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

## Conflict policy

Default policy is `session` (Linux-safe best practice): competing agents are stopped for the current session only.

Optional modes can be set with a service override environment variable:

- `NOCTALIA_AUTH_CONFLICT_MODE=session` (default)
- `NOCTALIA_AUTH_CONFLICT_MODE=persistent` (disable known competing user services/autostarts)
- `NOCTALIA_AUTH_CONFLICT_MODE=warn` (detect only)

In `persistent` mode, user autostart entries are backed up under `~/.local/state/noctalia-auth/autostart-backups/` before override files are written.

Example:

```bash
systemctl --user edit noctalia-auth.service
```

Add:

```ini
[Service]
Environment=NOCTALIA_AUTH_CONFLICT_MODE=session
```

## Smoke checks

```bash
pkexec true
echo test | secret-tool store --label=test attr val
```

If prompts do not appear, run `./build-dev.sh doctor` first.

For common failures, see `docs/TROUBLESHOOTING.md`.

## Nix

```bash
nix build .#noctalia-auth
nix profile install .#noctalia-auth
```
