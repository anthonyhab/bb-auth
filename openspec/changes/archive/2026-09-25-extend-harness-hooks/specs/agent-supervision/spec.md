# Spec Delta

## ADDED Requirements

### Requirement: Codex CLI PreToolUse coverage

The intent hook SHALL treat a payload carrying a non-empty `turn_id` (a
codex-specific field) as a Codex CLI `PreToolUse` event, handling `tool_name`
`"Bash"` or `"exec_command"` (or a `matcher_aliases` entry of `"Bash"`), and
declaring with `agent: "codex"`. A rewrite MUST be emitted in the codex
contract — `hookSpecificOutput` with `permissionDecision: "allow"` and
`updatedInput.command` — and only when the daemon acknowledged the declaration.

#### Scenario: Codex clean sudo rewrite

- **WHEN** codex invokes the hook with `tool_name: "Bash"`,
  `tool_input.command: "sudo x"`, and a `turn_id`
- **THEN** the hook declares intent with `agent: "codex"` and emits
  `hookSpecificOutput.updatedInput.command: "pkexec x"` with
  `permissionDecision: "allow"`

#### Scenario: Claude payload is not misdetected

- **WHEN** a payload has `tool_name: "Bash"` but no `turn_id` or other
  codex-specific discriminator
- **THEN** the hook treats it as Claude Code (`agent: "claude-code"`)

### Requirement: pi extension coverage

A pi extension SHALL register a `tool_call` handler that, for the `bash` tool
with a privileged leading command, declares intent to the daemon and mutates
`event.input.command` in place for a clean `sudo`/`doas` rewrite — only when
the daemon acknowledged. On any failure it MUST leave the call unchanged
(fail-open).

#### Scenario: pi rewrite via in-place mutation

- **WHEN** pi's `bash` tool is called with `command: "sudo x"` and the daemon
  answers `ok`
- **THEN** `event.input.command` is `"pkexec x"` when the tool executes and the
  intent was declared with `agent: "pi"`

#### Scenario: pi daemon-down passthrough

- **WHEN** the daemon socket is unreachable
- **THEN** the handler returns without mutating `event.input` and the original
  command runs unchanged

### Requirement: pi process detection

The daemon and shim signature sets SHALL recognize pi by the
`pi-coding-agent` cmdline path segment (the npm package directory). The bare
token `pi` SHALL NOT be a signature — it collides with unrelated binaries of
the same name.

#### Scenario: pi ancestor detected

- **WHEN** a command's ancestry includes a node process running
  `.../pi-coding-agent/.../cli.js`
- **THEN** the shim/daemon resolve the agent kind as `pi`

#### Scenario: unrelated pi binary not detected

- **WHEN** ancestry contains a process named exactly `pi` with no
  `pi-coding-agent` path segment
- **THEN** no agent is detected and invocation passes through as human
