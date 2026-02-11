#!/usr/bin/env bash
set -e

BUILD_DIR="build-dev"
PREFIX="$HOME/.local"
SERVICE_NAME="noctalia-auth.service"
SERVICE_SOURCE="$PREFIX/lib/systemd/user/$SERVICE_NAME"
SERVICE_DEST="$HOME/.config/systemd/user/$SERVICE_NAME"
SERVICE_OVERRIDE_DIR="$HOME/.config/systemd/user/${SERVICE_NAME}.d"
EXPECTED_PINENTRY="$PREFIX/libexec/pinentry-noctalia"
RUNTIME_SOCKET="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/noctalia-auth.sock"

doctor() {
    local ok=1

    echo "== noctalia-auth doctor =="

    if systemctl --user is-active --quiet "$SERVICE_NAME"; then
        echo "[ok] service active: $SERVICE_NAME"
    else
        echo "[fail] service not active: $SERVICE_NAME"
        ok=0
    fi

    local fragment
    fragment=$(systemctl --user show "$SERVICE_NAME" -p FragmentPath --value 2>/dev/null || true)
    if [ -n "$fragment" ]; then
        echo "[info] service unit: $fragment"
    else
        echo "[fail] could not resolve service unit path"
        ok=0
    fi

    if [ -x "$PREFIX/libexec/noctalia-auth" ]; then
        echo "[ok] daemon binary exists: $PREFIX/libexec/noctalia-auth"
    else
        echo "[fail] missing daemon binary: $PREFIX/libexec/noctalia-auth"
        ok=0
    fi

    if [ -x "$EXPECTED_PINENTRY" ]; then
        echo "[ok] pinentry binary exists: $EXPECTED_PINENTRY"
    else
        echo "[fail] missing pinentry binary: $EXPECTED_PINENTRY"
        ok=0
    fi

    if [ -x "$PREFIX/libexec/noctalia-auth-bootstrap" ]; then
        echo "[ok] bootstrap binary exists: $PREFIX/libexec/noctalia-auth-bootstrap"
    else
        echo "[fail] missing bootstrap binary: $PREFIX/libexec/noctalia-auth-bootstrap"
        ok=0
    fi

    if [ -x "$PREFIX/libexec/noctalia-auth-fallback" ]; then
        echo "[ok] fallback UI binary exists: $PREFIX/libexec/noctalia-auth-fallback"
    else
        echo "[warn] missing fallback UI binary: $PREFIX/libexec/noctalia-auth-fallback"
    fi

    if [ -S "$RUNTIME_SOCKET" ]; then
        echo "[ok] runtime socket present: $RUNTIME_SOCKET"
    else
        echo "[warn] runtime socket missing: $RUNTIME_SOCKET"
    fi

    if [ -n "$fragment" ] && grep -q 'noctalia-auth-bootstrap' "$fragment"; then
        echo "[ok] service includes bootstrap prestart"
    else
        echo "[fail] service missing bootstrap prestart"
        ok=0
    fi

    local gpg_conf="$HOME/.gnupg/gpg-agent.conf"
    if [ -f "$gpg_conf" ]; then
        local pinentry_line
        pinentry_line=$(grep -E '^[[:space:]]*pinentry-program[[:space:]]+' "$gpg_conf" | tail -n 1 || true)
        if [ -n "$pinentry_line" ]; then
            local configured
            configured=$(printf '%s' "$pinentry_line" | sed -E 's/^[[:space:]]*pinentry-program[[:space:]]+//')
            if [ "$configured" = "$EXPECTED_PINENTRY" ]; then
                echo "[ok] gpg-agent pinentry-program is canonical"
            else
                echo "[fail] gpg-agent pinentry-program points to: $configured"
                echo "       expected: $EXPECTED_PINENTRY"
                if printf '%s' "$configured" | grep -q '/build/'; then
                    echo "       stale build path detected"
                fi
                ok=0
            fi
        else
            echo "[warn] gpg-agent.conf has no pinentry-program entry"
            echo "       add: pinentry-program $EXPECTED_PINENTRY"
        fi
    else
        echo "[warn] missing $gpg_conf"
    fi

    if [ $ok -eq 1 ]; then
        echo "== doctor result: healthy =="
        return 0
    fi

    echo "== doctor result: issues detected =="
    return 1
}

fix_gpg_pinentry() {
    local gpg_dir="$HOME/.gnupg"
    local gpg_conf="$gpg_dir/gpg-agent.conf"

    mkdir -p "$gpg_dir"
    chmod 700 "$gpg_dir"

    if [ -f "$gpg_conf" ] && grep -qE '^[[:space:]]*pinentry-program[[:space:]]+' "$gpg_conf"; then
        sed -i -E "s|^[[:space:]]*pinentry-program[[:space:]]+.*$|pinentry-program $EXPECTED_PINENTRY|" "$gpg_conf"
    else
        printf '\npinentry-program %s\n' "$EXPECTED_PINENTRY" >> "$gpg_conf"
    fi

    chmod 600 "$gpg_conf"

    gpgconf --kill gpg-agent || true
    gpgconf --launch gpg-agent || true

    echo "Updated gpg-agent pinentry-program to: $EXPECTED_PINENTRY"
}

install_user_service_override() {
    if [ -f "$SERVICE_SOURCE" ]; then
        mkdir -p "$(dirname "$SERVICE_DEST")"
        mv -f "$SERVICE_SOURCE" "$SERVICE_DEST"
        echo "Installed user service override: $SERVICE_DEST"
    fi
}

cleanup_stale_service_overrides() {
    if [ -d "$SERVICE_OVERRIDE_DIR" ]; then
        rm -rf "$SERVICE_OVERRIDE_DIR"
        echo "Removed stale override directory: $SERVICE_OVERRIDE_DIR"
    fi
}

case "$1" in
    build)
        cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="$PREFIX"
        cmake --build "$BUILD_DIR"
        ;;
    install)
        cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="$PREFIX"
        cmake --build "$BUILD_DIR"
        DESTDIR="" cmake --install "$BUILD_DIR"
        install_user_service_override
        cleanup_stale_service_overrides
        ;;
    enable)
        cleanup_stale_service_overrides
        systemctl --user daemon-reload
        systemctl --user enable --now "$SERVICE_NAME"
        systemctl --user restart "$SERVICE_NAME"
        ;;
    disable)
        systemctl --user disable --now "$SERVICE_NAME"
        rm -f "$SERVICE_DEST"
        rm -rf "$HOME/.config/systemd/user/noctalia-auth.service.d"
        systemctl --user daemon-reload
        ;;
    status)
        systemctl --user status "$SERVICE_NAME"
        ;;
    doctor)
        doctor
        ;;
    fix-gpg)
        fix_gpg_pinentry
        ;;
    uninstall)
        rm -f "$SERVICE_DEST"
        rm -rf "$HOME/.config/systemd/user/noctalia-auth.service.d"
        systemctl --user daemon-reload 2>/dev/null || true

        rm -f "$PREFIX/libexec/noctalia-auth"
        rm -f "$PREFIX/libexec/noctalia-auth-bootstrap"
        rm -f "$PREFIX/libexec/noctalia-auth-fallback"
        rm -f "$PREFIX/libexec/noctalia-keyring-prompter"
        rm -f "$PREFIX/libexec/pinentry-noctalia"
        rm -f "$PREFIX/lib/systemd/user/noctalia-auth.service"
        rm -f "$PREFIX/share/dbus-1/services/org.noctalia.polkitagent.service"
        rm -f "$PREFIX/share/noctalia-auth/org.gnome.keyring.SystemPrompter.service"

        rm -rf "$BUILD_DIR"
        ;;
    *)
        echo "Usage: $0 {build|install|enable|disable|status|doctor|fix-gpg|uninstall}"
        echo ""
        echo "Commands:"
        echo "  build      - Build the project in $BUILD_DIR"
        echo "  install    - Build and install to $PREFIX (overrides $SERVICE_NAME in ~/.config/systemd/user/)"
        echo "  enable     - Enable and start $SERVICE_NAME"
        echo "  disable    - Disable and remove the user override for $SERVICE_NAME"
        echo "  status     - Show the status of the dev service"
        echo "  doctor     - Validate runtime wiring and gpg pinentry path"
        echo "  fix-gpg    - Configure gpg-agent to use $EXPECTED_PINENTRY"
        echo "  uninstall  - Remove dev build and installation"
        exit 1
        ;;
esac
