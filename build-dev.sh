#!/usr/bin/env bash
set -e

BUILD_DIR="build-dev"
PREFIX="$HOME/.local"
SERVICE_NAME="noctalia-auth.service"
SERVICE_SOURCE="$PREFIX/lib/systemd/user/$SERVICE_NAME"
SERVICE_DEST="$HOME/.config/systemd/user/$SERVICE_NAME"

install_user_service_override() {
    if [ -f "$SERVICE_SOURCE" ]; then
        mkdir -p "$(dirname "$SERVICE_DEST")"
        mv -f "$SERVICE_SOURCE" "$SERVICE_DEST"
        echo "Installed user service override: $SERVICE_DEST"
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
        ;;
    enable)
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
    uninstall)
        rm -f "$SERVICE_DEST"
        rm -rf "$HOME/.config/systemd/user/noctalia-auth.service.d"
        systemctl --user daemon-reload 2>/dev/null || true

        rm -f "$PREFIX/libexec/noctalia-auth"
        rm -f "$PREFIX/libexec/noctalia-keyring-prompter"
        rm -f "$PREFIX/libexec/pinentry-noctalia"
        rm -f "$PREFIX/lib/systemd/user/noctalia-auth.service"
        rm -f "$PREFIX/share/dbus-1/services/org.noctalia.polkitagent.service"
        rm -f "$PREFIX/share/noctalia-auth/org.gnome.keyring.SystemPrompter.service"

        rm -rf "$BUILD_DIR"
        ;;
    *)
        echo "Usage: $0 {build|install|enable|disable|status|uninstall}"
        echo ""
        echo "Commands:"
        echo "  build      - Build the project in $BUILD_DIR"
        echo "  install    - Build and install to $PREFIX (overrides $SERVICE_NAME in ~/.config/systemd/user/)"
        echo "  enable     - Enable and start $SERVICE_NAME"
        echo "  disable    - Disable and remove the user override for $SERVICE_NAME"
        echo "  status     - Show the status of the dev service"
        echo "  uninstall  - Remove dev build and installation"
        exit 1
        ;;
esac
