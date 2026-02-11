#!/usr/bin/env bash
# BB Auth Migration Script
# Run this once when upgrading from noctalia-auth/polkit-auth to bb-auth
# This script will be removed in a future release

set -euo pipefail

SCRIPT_NAME="bb-auth-migrate"
LEGACY_SERVICE="noctalia-auth.service"
NEW_SERVICE="bb-auth.service"

log() {
    printf '[%s] %s\n' "$SCRIPT_NAME" "$*"
}

log "Starting migration from noctalia-auth to bb-auth"

# 1. Stop and disable legacy service
if systemctl --user is-active "$LEGACY_SERVICE" &>/dev/null; then
    log "Stopping $LEGACY_SERVICE..."
    systemctl --user stop "$LEGACY_SERVICE" || true
fi

if systemctl --user is-enabled "$LEGACY_SERVICE" &>/dev/null; then
    log "Disabling $LEGACY_SERVICE..."
    systemctl --user disable "$LEGACY_SERVICE" || true
fi

# 2. Remove user systemd service overrides
LEGACY_OVERRIDE_DIR="$HOME/.config/systemd/user/noctalia-auth.service.d"
if [ -d "$LEGACY_OVERRIDE_DIR" ]; then
    log "Removing legacy service overrides..."
    rm -rf "$LEGACY_OVERRIDE_DIR"
fi

# 3. Clean up legacy runtime files
LEGACY_SOCKET="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/noctalia-auth.sock"
if [ -e "$LEGACY_SOCKET" ]; then
    log "Removing legacy socket..."
    rm -f "$LEGACY_SOCKET"
fi

LEGACY_LOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/noctalia-auth-fallback.lock"
if [ -e "$LEGACY_LOCK" ]; then
    log "Removing legacy lock file..."
    rm -f "$LEGACY_LOCK"
fi

# 4. Backup and clean legacy state
LEGACY_STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/noctalia-auth"
if [ -d "$LEGACY_STATE_DIR" ]; then
    BACKUP_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/noctalia-auth-backup-$(date +%Y%m%d-%H%M%S)"
    log "Backing up legacy state to $BACKUP_DIR..."
    cp -r "$LEGACY_STATE_DIR" "$BACKUP_DIR"
    log "Removing legacy state directory..."
    rm -rf "$LEGACY_STATE_DIR"
fi

# 5. Find and report legacy binaries
LEGACY_BINARIES=(
    "$HOME/.local/libexec/noctalia-auth"
    "$HOME/.local/libexec/noctalia-auth-fallback"
    "$HOME/.local/libexec/noctalia-auth-bootstrap"
    "$HOME/.local/libexec/noctalia-keyring-prompter"
    "$HOME/.local/libexec/pinentry-noctalia"
    "$HOME/.local/libexec/noctalia-polkit"  # Very old
)

FOUND_LEGACY=()
for binary in "${LEGACY_BINARIES[@]}"; do
    if [ -e "$binary" ]; then
        FOUND_LEGACY+=("$binary")
    fi
done

if [ ${#FOUND_LEGACY[@]} -gt 0 ]; then
    log "Found legacy binaries that should be manually removed:"
    for binary in "${FOUND_LEGACY[@]}"; do
        echo "  - $binary"
    done
    echo ""
    log "To remove them automatically, run:"
    echo "  bb-auth-migrate --remove-binaries"
fi

# 6. Check for legacy D-Bus service files
LEGACY_DBUS_SERVICE="$HOME/.local/share/dbus-1/services/org.noctalia.polkitagent.service"
if [ -e "$LEGACY_DBUS_SERVICE" ]; then
    log "Removing legacy D-Bus service file..."
    rm -f "$LEGACY_DBUS_SERVICE"
fi

# 7. Check for autostart overrides that reference old names
AUTOSTART_DIR="$HOME/.config/autostart"
if [ -d "$AUTOSTART_DIR" ]; then
    for file in "$AUTOSTART_DIR"/*.desktop; do
        if [ -f "$file" ] && grep -q "noctalia-auth\|polkit-auth" "$file" 2>/dev/null; then
            log "Found legacy reference in: $file"
            log "You may want to review and update this file manually"
        fi
    done
fi

# Handle --remove-binaries flag
if [ "${1:-}" = "--remove-binaries" ] && [ ${#FOUND_LEGACY[@]} -gt 0 ]; then
    log "Removing legacy binaries..."
    for binary in "${FOUND_LEGACY[@]}"; do
        rm -f "$binary"
        log "Removed: $binary"
    done
fi

# 8. Reload systemd daemon
log "Reloading systemd daemon..."
systemctl --user daemon-reload

log ""
log "=========================================="
log "Migration complete!"
log "=========================================="
log ""
log "Next steps:"
log "1. Install bb-auth (if not already installed):"
log "   cmake --build build --target install"
log ""
log "2. Enable the new service:"
log "   systemctl --user enable --now $NEW_SERVICE"
log ""
log "3. Update your Noctalia plugin:"
log "   - Remove the old 'polkit-auth' plugin"
log "   - Install the new 'bb-auth' plugin"
log ""
log "4. Test the setup:"
log "   pkexec true"
log ""

if [ -d "$LEGACY_STATE_DIR" ] 2>/dev/null; then
    log "Note: Legacy state backed up to: $BACKUP_DIR"
fi
