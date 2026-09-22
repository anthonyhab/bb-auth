# Proposal

## Why

The voluntary declare CLI is the channel agents reach for first, but its
grammar is wordy (`--reason` is mandatory free text), it fails silently into a
guaranteed-loss path for the most common agent idiom (`sudo -n CMD` declines
the rewrite, then real sudo refuses to prompt), and there is no structured
input for harnesses that would rather emit JSON than shell-quote a command
line. The name `bb-auth-declare` also describes the mechanism instead of the
action the caller wants.

## What Changes

- **New canonical name `aisudo`** installed beside `bb-auth-declare`; the old
  name remains a permanent alias so existing skills, docs, and muscle memory
  keep working.
- **Optional reason**: `--reason`/`-r` defaults to the command string when a
  command is given. Only a declare-only invocation (no command) still requires
  an explicit reason.
- **`--json` input mode**: accepts an inline JSON object or `-` for stdin.
  Schema: `{"argv": [...], "command": "...", "reason"?, "agent"?, "cwd"?}` —
  `argv` (array, exact) or `command` (string, shlex-split); unknown keys and
  malformed input fail with a corrective error naming the fix.
- **`--dry-run`**: prints the resolved plan (declared payload + exec argv) as
  JSON and exits without declaring or executing — the "assume shape" probe.
- **Supported `sudo` option subset translates instead of declining**:
  `-n`/`--non-interactive` is dropped (the supervised GUI prompt *is* the
  non-interactive path), `-u`/`--user` maps to `pkexec --user`, `--` is
  consumed. All other options, env assignments, and shell metacharacters still
  decline — shim passes through, CLI warns on stderr with the specific reason.
- **One shared escalation translator** for the CLI and PATH-shim paths,
  replacing the near-duplicate `_rewrite_sudo`/`_shim_rewrite` pair.
- **Self-correcting errors**: every misuse prints the exact corrected
  invocation; `--help` epilog documents the full contract so it stays the
  single source of truth.

## Capabilities

### New Capabilities

- `declare-cli`: the voluntary CLI contract — name, grammar, JSON input,
  reason defaulting, dry-run, error behavior, and the sudo translation subset
  as applied to explicit invocation.

### Modified Capabilities

- `agent-supervision`: the "Escalation rewrite and declaration" requirement
  currently mandates that all option-bearing commands decline the rewrite;
  it now carves out the supported `sudo` subset (`-n`, `-u`/`--user`, `--`)
  that translates to `pkexec`, with probe-only invocations (no command) still
  declining.

## Impact

- `integrations/agent-cli/aisudo.in` — grammar, JSON mode, shared
  translator, help text.
- `CMakeLists.txt` — install the same program a second time as `aisudo`.
- `tests/test_declare_shim.py` — updated rewrite expectations plus new CLI
  cases (JSON, dry-run, default reason, corrective errors).
- `README.md`, `lat.md/agent-intent.md`, `lat.md/architecture.md` — name and
  grammar references.
- `~/.agents/skills/bb-auth/SKILL.md` (outside repo) — shrink to the `aisudo`
  one-liner plus pointers to `--help`.
- No daemon or provider-protocol changes; `intent.declare` payload unchanged.
