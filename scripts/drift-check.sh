#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

echo "== runtime drift check =="

legacy_runtime_refs=$(git grep -nE \
  'noctalia-polkit\.service|bb-auth\.sock|bb-auth|"type"[[:space:]]*:[[:space:]]*"respond"|"type"[[:space:]]*:[[:space:]]*"cancel"' \
  -- . ':!scripts/drift-check.sh' ':!README.md' ':!PKGBUILD' ':!flake.nix' ':!nix/default.nix' ':!nix/overlays.nix' || true)

if [ -n "$legacy_runtime_refs" ]; then
  echo "[fail] legacy runtime identifiers found:"
  echo "$legacy_runtime_refs"
  exit 1
fi

if [ -e assets/bb-auth.service.in ]; then
  echo "[fail] stale asset still tracked: assets/bb-auth.service.in"
  exit 1
fi

if [ -e assets/bb-auth-dbus.service.in ]; then
  echo "[fail] stale asset still tracked: assets/bb-auth-dbus.service.in"
  exit 1
fi

if grep -q 'cp -n' assets/bb-auth.service.in; then
  echo "[fail] stale cp -n behavior found in assets/bb-auth.service.in"
  exit 1
fi

echo "[ok] no legacy runtime drift detected"
