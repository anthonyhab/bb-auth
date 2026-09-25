# Spec Delta

## ADDED Requirements

### Requirement: Single compiled agent binary

All agent-facing tools (harness hook, `aisudo`, the `sudo`/`doas`/`pkexec`
PATH shims, and `bb-auth-agents`) SHALL be one compiled binary selected by
the basename of `argv[0]`, and no installed agent-facing tool MAY require an
interpreter at runtime. Agent recognition SHALL use the same implementation
as the daemon's requestor resolver.

#### Scenario: Shim runs without Python

- **WHEN** an agent runs `sudo id` with the shim directory first on PATH on a
  system without Python installed
- **THEN** the shim resolves, declares, and translates exactly as before

#### Scenario: Config edits preserve the user's file

- **WHEN** `bb-auth-agents install` then `uninstall` run against a
  conventionally formatted settings file
- **THEN** the file is byte-for-byte identical to the original
