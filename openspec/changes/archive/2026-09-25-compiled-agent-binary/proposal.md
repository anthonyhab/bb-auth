# Proposal

## Why

`aisudo` (with its shim mode) and `bb-auth-agents` were Python scripts. That
put an interpreter on every agent `sudo` (~27 ms vs ~2 ms), made the shim
depend on whichever `python3` `/usr/bin/env` found first (a venv could shadow
it), kept a second copy of the daemon's agent-alias table, and made Python a
runtime dependency of a security utility.

## What Changes

- One Qt6::Core binary, `bb-auth-agent`, behind every agent-facing name via
  argv[0] dispatch: `bb-auth-intent-hook`, `aisudo`, `bb-auth-agents`, and the
  `sudo`/`doas`/`pkexec` shims (all symlinks). Installed paths are unchanged.
- Agent detection moves to `src/common/ProcAgent` and is shared with the
  daemon — one alias table.
- An order-preserving JSON layer for editing user configs (Qt sorts keys).
- Python becomes a test-only dependency; behavior and CLI grammar unchanged.

## Impact

`integrations/agent/` replaces `integrations/hooks/`, `integrations/agent-cli/`,
and `integrations/agents/`; packaging drops the `python` optdepend.
