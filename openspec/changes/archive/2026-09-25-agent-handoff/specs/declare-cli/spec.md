# Spec Delta

## MODIFIED Requirements

### Requirement: Invocation surface

The CLI SHALL be installed under the name `aisudo` and MUST accept the
grammar: `[--reason TEXT|-r TEXT] [--agent NAME] [--json JSON|-]
[--dry-run] [--print-shim-dir] [--] [LAUNCHER] COMMAND...`. The `--`
separator is optional, and LAUNCHER (`sudo`/`doas`/`pkexec`) is optional —
a bare command is treated exactly as `sudo COMMAND` for declaration and
translation purposes.

#### Scenario: Minimal invocation

- **WHEN** an agent runs `aisudo sudo make install`
- **THEN** intent is declared and the resolved command is exec'd

#### Scenario: Bare command elevates like sudo

- **WHEN** an agent runs `aisudo make install` with a live daemon
- **THEN** it behaves as `aisudo sudo make install`: `pkexec make install` is
  exec'd; with no daemon answer it falls back to `sudo make install`

#### Scenario: Compatibility name

- **WHEN** the package is installed
- **THEN** only `aisudo` is installed in BINDIR; no `bb-auth-declare` alias
  exists, so agents and docs see one name
