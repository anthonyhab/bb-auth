# Spec Delta

## MODIFIED Requirements

### Requirement: Escalation rewrite and declaration

Under a recognized agent, the shim SHALL declare intent to the daemon with
`channel: "shim"` and a generic reason, and SHALL rewrite a clean leading
`sudo CMD` or `doas CMD` to `pkexec CMD` so the escalation reaches the
supervised polkit prompt. The supported `sudo` option subset MUST also
rewrite: `-n`/`--non-interactive` is dropped (the supervised GUI prompt is
the non-interactive path), `-u`/`--user USER` maps to `pkexec --user USER`,
and `--` is consumed. A `sudo` invocation bearing only options and no command
is a probe and MUST NOT be rewritten. Commands bearing other options,
environment assignments, or shell metacharacters MUST NOT be rewritten —
they exec the real binary unchanged. The declared reason MUST NOT contain
the command argv or any user-supplied string.

#### Scenario: Clean sudo rewrite

- **WHEN** an agent process resolves `sudo pacman -Syu` through the shim
- **THEN** the shim declares intent (`channel: "shim"`) and execs
  `pkexec pacman -Syu`, producing a supervised, attributed bb-auth prompt

#### Scenario: Non-interactive sudo rewrite

- **WHEN** an agent runs `sudo -n make install` through the shim
- **THEN** the shim declares intent and execs `pkexec make install` — the `-n`
  flag is dropped because the supervised prompt replaces stdin auth

#### Scenario: Sudo user flag maps to pkexec

- **WHEN** an agent runs `sudo -u nobody id` through the shim
- **THEN** the shim declares intent and execs `pkexec --user nobody id`

#### Scenario: Probe-only sudo passes through

- **WHEN** an agent runs `sudo -nv` or `sudo -n` with no command through the
  shim
- **THEN** the command is not rewritten; the shim execs the real `sudo` so the
  credential probe keeps its original semantics

#### Scenario: Option-bearing sudo passes through

- **WHEN** an agent runs `sudo -E id` through the shim
- **THEN** the command is not rewritten; the shim execs the real `sudo` and the
  escalation proceeds via sudo's normal path (unsupervised, as before)

#### Scenario: Reason is generic and secret-free

- **WHEN** the shim declares intent
- **THEN** the reason is a fixed generic string (no argv, no environment data),
  and the invoked command is sent only in the audit-only `command` field which
  prompts never render
