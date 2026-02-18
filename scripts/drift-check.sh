#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

echo "== runtime drift check =="

# Check for legacy noctalia naming (these should not exist in codebase)
# Exclude files that intentionally reference the old repo name (github URLs, AUR refs, etc.)
legacy_runtime_refs=$(git grep -nE \
  'noctalia-auth|noctalia-polkit|pinentry-noctalia|noctalia-keyring-prompter|noctalia-prompt|org\.noctalia\.polkitagent' \
  -- . ':!scripts/drift-check.sh' ':!README.md' ':!docs/*.md' ':!PKGBUILD' ':!nix/*.nix' || true)

if [ -n "$legacy_runtime_refs" ]; then
  echo "[fail] legacy runtime identifiers found:"
  echo "$legacy_runtime_refs"
  exit 1
fi

# Check for old stale asset files (should not exist after rebrand)
if [ -e assets/noctalia-auth.service.in ]; then
  echo "[fail] stale asset still tracked: assets/noctalia-auth.service.in"
  exit 1
fi

if [ -e assets/noctalia-auth-dbus.service.in ]; then
  echo "[fail] stale asset still tracked: assets/noctalia-auth-dbus.service.in"
  exit 1
fi

# Check for stale cp -n behavior in current service file
if [ -f assets/bb-auth.service.in ] && grep -q 'cp -n' assets/bb-auth.service.in; then
  echo "[fail] stale cp -n behavior found in assets/bb-auth.service.in"
  exit 1
fi

echo "[ok] no legacy runtime drift detected"
