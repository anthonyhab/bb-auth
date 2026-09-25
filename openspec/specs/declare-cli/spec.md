# declare-cli Specification

## Purpose
The voluntary declaration channel: a single CLI that agents and scripts wrap
around a privileged command to attach who/why attribution and route supported
escalations through bb-auth's supervised polkit prompt.

## Requirements

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

### Requirement: Reason defaulting

`--reason` SHALL be optional when a command is present, defaulting to the
joined command string. A declare-only invocation (no command) MUST still
require an explicit reason.

#### Scenario: Reason derived from command

- **WHEN** `aisudo -- sudo make install` runs without `--reason`
- **THEN** the declared reason is the command string `sudo make install`

#### Scenario: Declare-only still requires reason

- **WHEN** `aisudo` runs with no command and no `--reason`
- **THEN** it exits non-zero with an error stating that `--reason` is required
  when no command is given

### Requirement: JSON input mode

`--json` SHALL accept either an inline JSON object or `-` to read the object
from stdin. The object MUST contain `argv` (array of strings) or `command`
(string, split shell-style); `reason`, `agent`, and `cwd` are optional.
Malformed JSON, a missing command field, and unknown keys MUST each fail with
a non-zero exit and an error naming the expected shape.

#### Scenario: Structured call

- **WHEN** `aisudo --json '{"argv": ["sudo","make","install"], "reason": "install omafox"}'`
- **THEN** the declaration carries reason `install omafox` and the exec'd
  command is the translated argv

#### Scenario: Corrective error on bad input

- **WHEN** `aisudo --json '{"args": ["sudo","id"]}'`
- **THEN** it exits non-zero and the error names the accepted keys
  (`argv`, `command`, `reason`, `agent`, `cwd`)

### Requirement: Dry-run plan

`--dry-run` SHALL print the resolved plan as a JSON object — the declaration
payload and the final exec argv after translation — and exit zero without
contacting the daemon or executing anything.

#### Scenario: Shape inspection

- **WHEN** `aisudo --dry-run -- sudo -n make install`
- **THEN** stdout shows the `pkexec` translation and the declaration payload,
  and nothing is executed

### Requirement: Escalation translation parity

The CLI SHALL apply the same escalation translation rules as the PATH shim:
a clean leading `sudo CMD`/`doas CMD`, including the supported `sudo` option
subset (`-n`/`--non-interactive`, `-u`/`--user`, `--`), resolves to `pkexec`;
all other option-bearing or metacharacter-bearing commands are left unchanged
with a stderr note stating why. As with the shim, translation MUST only apply
when the daemon bound the declaration; otherwise the command execs unchanged
(fail-open).

#### Scenario: Non-interactive sudo translates

- **WHEN** `aisudo -- sudo -n make install` runs with a live daemon
- **THEN** `pkexec make install` is exec'd and stderr notes that `-n` was
  dropped because auth happens at the GUI prompt

#### Scenario: Unsupported option declines with reason

- **WHEN** `aisudo -- sudo -E env CMD` runs
- **THEN** the command execs via `sudo` unchanged and stderr states the option
  is not translatable to pkexec

### Requirement: Self-correcting misuse errors

Every argument-parsing or input failure SHALL exit non-zero with a message
that includes the corrected invocation form, and `--help` SHALL document the
full grammar, the JSON schema, and the supported `sudo` option subset.

#### Scenario: Missing command flags the fix

- **WHEN** `aisudo --reason "x"` runs with no command
- **THEN** the error prints the canonical form `aisudo [-r WHY] [--] CMD...`
