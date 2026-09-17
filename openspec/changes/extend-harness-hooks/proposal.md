# Extend harness hook coverage: Codex CLI + pi

## Why

The intent hook currently detects Claude Code, Devin, and Gemini CLI, and ships a
plugin for opencode. Codex CLI now ships a Claude-compatible `PreToolUse` hook
framework (`hooks.json` / `[hooks]` in `config.toml`), and pi ships an
extension API whose `tool_call` event can mutate `event.input` in place. Without
coverage, escalations from those agents rely on the PATH shim alone — they get
supervision but never a real declared reason, and a shim-less PATH gets nothing.

## What changes

- `bb-auth-intent-hook` gains Codex CLI detection: codex payloads carry
  `turn_id`/`tool_use_id` discriminators (Claude-compatible `tool_name: "Bash"`,
  `tool_input.command`); the emitted `hookSpecificOutput` envelope already
  matches codex's `permissionDecision: "allow"` + `updatedInput` contract.
- New `integrations/pi/` extension: `tool_call` handler that declares intent and
  mutates `event.input.command` for a clean `sudo`/`doas` rewrite, gated on a
  daemon `ok` reply, fail-open like the opencode plugin.
- Agent signature sets (daemon + shim) gain `pi-coding-agent` (npm package path
  segment); bare `pi` is intentionally NOT a signature (name collision risk).
- Docs: hooks README gains Codex (`hooks.json` + `/hooks` trust review) and pi
  sections; lat.md harness list updated.

## Capabilities

- `agent-supervision` (modified — ADDED requirements for codex/pi hook coverage)

## Out of scope

- opencode plugin changes (verified already correct against current docs).
- `PermissionRequest`/`PostToolUse` codex events, pi `block` returns — this
  change stays declare + rewrite only.
