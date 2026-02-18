#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-$(nproc)}"

BUILD_CHECK_DIR="${BUILD_CHECK_DIR:-$ROOT_DIR/build-check}"
BUILD_CORE_DIR="${BUILD_CORE_DIR:-$ROOT_DIR/build-core}"
INSTALL_SMOKE_DIR="${INSTALL_SMOKE_DIR:-$ROOT_DIR/build-install-smoke}"

RUN_CORE=1
RUN_INSTALL_SMOKE=1
RUN_DAEMON_SMOKE=1
RUN_AUR_SMOKE=0
RUN_DEPLOY_LOCAL=0
STRICT_DAEMON_SMOKE="${STRICT_DAEMON_SMOKE:-0}"

usage() {
    cat <<'EOF'
Usage: scripts/gate-local.sh [options]

Runs local release gates before merging/pushing to main.

Options:
  --quick              Skip build-core, install smoke, and daemon smoke.
  --aur-smoke          Run optional local AUR PKGBUILD smoke (makepkg).
  --deploy-local       Build local package and install via pacman -U.
  --skip-core          Skip build-core configure/build/ctest gate.
  --skip-install-smoke Skip install smoke gate.
  --skip-daemon-smoke  Skip daemon socket ping smoke gate.
  -h, --help           Show this help.

Env:
  STRICT_DAEMON_SMOKE=1  Fail if daemon IPC bind is unavailable. Default skips
                         this gate when local session policy prevents binding.
EOF
}

log_step() {
    printf '\n==> %s\n' "$1"
}

require_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing required command: $cmd" >&2
        exit 1
    fi
}

run_build_gate() {
    local label="$1"
    local build_dir="$2"

    log_step "Gate [$label]: configure"
    cmake -S "$ROOT_DIR" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release

    log_step "Gate [$label]: build"
    cmake --build "$build_dir" -j"$JOBS"

    log_step "Gate [$label]: test"
    ctest --test-dir "$build_dir" --output-on-failure
}

run_install_smoke() {
    local build_dir="$1"
    local prefix="$2/prefix"

    rm -rf "$2"
    mkdir -p "$2"

    log_step "Gate [install-smoke]: cmake --install"
    cmake --install "$build_dir" --prefix "$prefix"

    local daemon_bin
    local fallback_bin
    local dbus_service
    local keyring_prompter_service

    daemon_bin="$(find "$prefix" -type f -name bb-auth -perm -u+x | head -n 1 || true)"
    fallback_bin="$(find "$prefix" -type f -name bb-auth-fallback -perm -u+x | head -n 1 || true)"
    dbus_service="$(find "$prefix" -type f -path '*/dbus-1/services/org.bb.auth.service' | head -n 1 || true)"
    keyring_prompter_service="$(find "$prefix" -type f -path '*/bb-auth/org.gnome.keyring.SystemPrompter.service' | head -n 1 || true)"

    if [[ -z "$daemon_bin" || -z "$fallback_bin" || -z "$dbus_service" || -z "$keyring_prompter_service" ]]; then
        echo "Install smoke failed: expected installed artifacts are missing." >&2
        echo "daemon_bin=$daemon_bin" >&2
        echo "fallback_bin=$fallback_bin" >&2
        echo "dbus_service=$dbus_service" >&2
        echo "keyring_prompter_service=$keyring_prompter_service" >&2
        exit 1
    fi
}

run_daemon_smoke() {
    local daemon_bin="$1"
    local socket_path="/tmp/bb-auth-pre-main-$$.sock"
    local log_path="$ROOT_DIR/build-daemon-smoke.log"
    local daemon_pid=""
    local ready=0

    if [[ ! -x "$daemon_bin" ]]; then
        echo "Daemon smoke failed: binary not found at $daemon_bin" >&2
        exit 1
    fi

    rm -f "$socket_path"

    log_step "Gate [daemon-smoke]: start daemon"
    BB_AUTH_SKIP_POLKIT=1 "$daemon_bin" --daemon --socket "$socket_path" >"$log_path" 2>&1 &
    daemon_pid="$!"

    for _ in $(seq 1 60); do
        if "$daemon_bin" --ping --socket "$socket_path" >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 0.1
    done

    if [[ "$ready" -ne 1 ]]; then
        if grep -q "Failed to start IPC server" "$log_path" && [[ "$STRICT_DAEMON_SMOKE" != "1" ]]; then
            echo "Daemon smoke skipped: IPC bind unavailable in this session." >&2
            echo "Set STRICT_DAEMON_SMOKE=1 to fail hard instead of skipping." >&2
            cat "$log_path" >&2 || true
            kill "$daemon_pid" >/dev/null 2>&1 || true
            wait "$daemon_pid" >/dev/null 2>&1 || true
            rm -f "$socket_path"
            return 0
        fi

        echo "Daemon smoke failed: ping did not succeed." >&2
        echo "Daemon log:" >&2
        cat "$log_path" >&2 || true
        kill "$daemon_pid" >/dev/null 2>&1 || true
        wait "$daemon_pid" >/dev/null 2>&1 || true
        rm -f "$socket_path"
        exit 1
    fi

    "$daemon_bin" --next --socket "$socket_path" >/dev/null 2>&1 || true
    kill "$daemon_pid" >/dev/null 2>&1 || true
    wait "$daemon_pid" >/dev/null 2>&1 || true
    rm -f "$socket_path"
}

run_aur_smoke() {
    require_cmd makepkg

    local tmp_dir
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "$tmp_dir"' RETURN

    log_step "Gate [aur-smoke]: makepkg from local checkout"
    build_local_pkg "$tmp_dir" >/dev/null

    rm -rf "$tmp_dir"
    trap - RETURN
}

build_local_pkg() {
    local work_dir="$1"

    rm -rf "$work_dir"
    mkdir -p "$work_dir"

    cp "$ROOT_DIR/PKGBUILD" "$work_dir/PKGBUILD"
    cp "$ROOT_DIR/LICENSE" "$work_dir/LICENSE"

    awk -v root="$ROOT_DIR" '
        BEGIN { replaced=0 }
        /^source=\(/ {
            print "source=(\"${pkgname}::git+file://" root "\")"
            replaced=1
            next
        }
        { print }
        END {
            if (!replaced) {
                exit 2
            }
        }
    ' "$ROOT_DIR/PKGBUILD" > "$work_dir/PKGBUILD.local"
    mv "$work_dir/PKGBUILD.local" "$work_dir/PKGBUILD"

    (
        cd "$work_dir"
        makepkg -s --noconfirm --nocheck
    )

    local pkg_path
    pkg_path="$(ls -1t "$work_dir"/*.pkg.tar.* | head -n 1 || true)"
    if [[ -z "$pkg_path" ]]; then
        echo "Failed to locate generated package in $work_dir" >&2
        exit 1
    fi

    echo "$pkg_path"
}

run_deploy_local() {
    require_cmd makepkg
    require_cmd pacman

    local deploy_dir="$ROOT_DIR/build-local-pkg"
    local pkg_path

    log_step "Gate [deploy-local]: build package"
    pkg_path="$(build_local_pkg "$deploy_dir")"
    echo "Package: $pkg_path"

    log_step "Gate [deploy-local]: install package"
    if [[ "$EUID" -eq 0 ]]; then
        pacman -U --noconfirm "$pkg_path"
    elif command -v sudo >/dev/null 2>&1; then
        sudo pacman -U --noconfirm "$pkg_path"
    else
        echo "Cannot install package: sudo not found and not running as root." >&2
        exit 1
    fi

    log_step "Gate [deploy-local]: restart user service"
    systemctl --user daemon-reload || true
    systemctl --user restart bb-auth.service || true
    systemctl --user --no-pager --full status bb-auth.service || true
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            RUN_CORE=0
            RUN_INSTALL_SMOKE=0
            RUN_DAEMON_SMOKE=0
            shift
            ;;
        --aur-smoke)
            RUN_AUR_SMOKE=1
            shift
            ;;
        --deploy-local)
            RUN_DEPLOY_LOCAL=1
            shift
            ;;
        --skip-core)
            RUN_CORE=0
            shift
            ;;
        --skip-install-smoke)
            RUN_INSTALL_SMOKE=0
            shift
            ;;
        --skip-daemon-smoke)
            RUN_DAEMON_SMOKE=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

require_cmd cmake
require_cmd ctest
require_cmd git

run_build_gate "build-check" "$BUILD_CHECK_DIR"

if [[ "$RUN_CORE" -eq 1 ]]; then
    run_build_gate "build-core" "$BUILD_CORE_DIR"
fi

if [[ "$RUN_INSTALL_SMOKE" -eq 1 ]]; then
    run_install_smoke "$BUILD_CHECK_DIR" "$INSTALL_SMOKE_DIR"
fi

if [[ "$RUN_DAEMON_SMOKE" -eq 1 ]]; then
    run_daemon_smoke "$BUILD_CHECK_DIR/bb-auth"
fi

if [[ "$RUN_AUR_SMOKE" -eq 1 ]]; then
    run_aur_smoke
fi

if [[ "$RUN_DEPLOY_LOCAL" -eq 1 ]]; then
    run_deploy_local
fi

log_step "All pre-main gates passed"
