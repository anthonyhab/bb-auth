#!/usr/bin/env bash
set -e

BUILD_DIR="build-dev"
PREFIX="$HOME/.local"
PROJECT_NAME="noctalia-polkit"
SERVICE_NAME="noctalia-polkit.service"
SERVICE_SOURCE="$PREFIX/lib/systemd/user/$SERVICE_NAME"
SERVICE_DEST="$HOME/.config/systemd/user/$SERVICE_NAME"

move_service_file() {
    if [ -f "$SERVICE_SOURCE" ]; then
        mkdir -p "$(dirname "$SERVICE_DEST")"
        mv "$SERVICE_SOURCE" "$SERVICE_DEST"
        echo "Moved service file to $SERVICE_DEST"
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
        move_service_file
        ;;
    enable)
        systemctl --user daemon-reload
        systemctl --user enable --now "$SERVICE_NAME"
        systemctl --user restart "$SERVICE_NAME"
        ;;
    disable)
        systemctl --user disable --now "$SERVICE_NAME"
        if [ -f "$SERVICE_DEST" ]; then
            rm -f "$SERVICE_DEST"
            systemctl --user daemon-reload
        fi
        ;;
    status)
        systemctl --user status "$SERVICE_NAME"
        ;;
    uninstall)
        if [ -f "$SERVICE_DEST" ]; then
            rm -f "$SERVICE_DEST"
        fi
        if [ -d "$BUILD_DIR" ]; then
            DESTDIR="" cmake --install "$BUILD_DIR" --component Unspecified 2>/dev/null || true
        fi
        rm -rf "$BUILD_DIR"
        ;;
    *)
        echo "Usage: $0 {build|install|enable|disable|status|uninstall}"
        echo ""
        echo "Commands:"
        echo "  build      - Build the project in $BUILD_DIR"
        echo "  install    - Build and install to $PREFIX (service to ~/.config/systemd/user/)"
        echo "  enable     - Enable and start the dev systemd user service"
        echo "  disable    - Disable and stop the dev systemd user service"
        echo "  status     - Show the status of the dev service"
        echo "  uninstall  - Remove dev build and installation"
        exit 1
        ;;
esac
