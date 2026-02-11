# bb-auth

Unified authentication daemon for Noctalia Shell.

- Polkit authentication agent
- GNOME Keyring system prompter replacement
- GPG pinentry bridge

This daemon is consumed by the `bb-auth` plugin in `noctalia-plugins`.

## Runtime Contract

- Service: `bb-auth.service`
- Socket: `$XDG_RUNTIME_DIR/bb-auth.sock`
- Main binary: `bb-auth`
- Pinentry binary: `pinentry-bb`
- Fallback UI binary: `bb-auth-fallback`

## Install

### AUR

```bash
yay -S bb-auth-git
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
systemctl --user enable --now bb-auth.service
```

On each service start, bootstrap automatically:

- validates and repairs `gpg-agent` pinentry path when stale
- stops known competing polkit agents for the current session

If no shell UI provider is active when an auth request arrives, daemon launches `bb-auth-fallback` automatically.
The fallback app enforces a single-instance lock and stands down when a higher-priority shell provider becomes active.

For tiling compositors, the fallback app uses a normal top-level window so it can still be tiled manually by the compositor.
On Hyprland, add a class-based rule if you want default floating + centering on open:

```ini
windowrule {
  name = Noctalia Auth Fallback
  float = on
  center = on
  size = 560 360
  match:class = bb-auth-fallback
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

- `BB_AUTH_CONFLICT_MODE=session` (default)
- `BB_AUTH_CONFLICT_MODE=persistent` (disable known competing user services/autostarts)
- `BB_AUTH_CONFLICT_MODE=warn` (detect only)

In `persistent` mode, user autostart entries are backed up under `~/.local/state/bb-auth/autostart-backups/` before override files are written.

Example:

```bash
systemctl --user edit bb-auth.service
```

Add:

```ini
[Service]
Environment=BB_AUTH_CONFLICT_MODE=session
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
nix build .#bb-auth
nix profile install .#bb-auth
```

## Rebrand Notice

Previously known as `noctalia-auth` (daemon) and `polkit-auth` (plugin).  
Renamed to BB Auth as part of the "habibe → bibe → BB" branding.  

### Upgrading from noctalia-auth

If you were using the previous version, run the migration script after installing:

```bash
bb-auth-migrate
# Or to automatically remove legacy binaries:
bb-auth-migrate --remove-binaries
```

This will:
- Stop and disable the old `noctalia-auth.service`
- Clean up legacy runtime files and state
- Back up your old state directory
- Report any legacy binaries that need manual removal

Then enable the new service:

```bash
systemctl --user enable --now bb-auth
```

The plugin ID has changed from `polkit-auth` to `bb-auth`. Reinstall through the Noctalia plugin UI.
