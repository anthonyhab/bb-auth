# Spec Delta

## Purpose

Environmental supervision of agent-initiated privilege escalation: PATH shims that
declare intent and route `sudo`/`doas` through bb-auth's supervised prompt without
requiring any cooperation from the agent, while remaining transparent to humans.

## ADDED Requirements

### Requirement: Agent-gated activation

The shim SHALL determine whether it runs under a recognized AI agent by walking
the invoking process's `/proc` ancestry against the known-agent signature set.
When no agent ancestor is found, the shim MUST behave as a transparent
passthrough: it SHALL locate the real binary by scanning `PATH` for the first
entry resolving to a different file than the shim itself, and exec it with the
original argv unchanged.

#### Scenario: Human invocation passes through

- **WHEN** a user runs `sudo -k` or any `sudo`/`doas`/`pkexec` command from a
  terminal with no agent ancestor
- **THEN** the shim execs the real binary with argv byte-for-byte unchanged and
  no declaration is emitted

#### Scenario: Agent invocation is intercepted

- **WHEN** a recognized agent (per the signature set) is an ancestor of the
  invoking process
- **THEN** the shim applies the declaration and rewrite rules instead of passing
  through directly

#### Scenario: Unknown process tree passes through

- **WHEN** ancestry contains no recognized agent signature (e.g. a plain shell
  script, a GUI app, a cron job)
- **THEN** the shim passes through exactly as for a human invocation

### Requirement: Escalation rewrite and declaration

Under a recognized agent, the shim SHALL declare intent to the daemon with
`channel: "shim"` and a generic reason, and SHALL rewrite a clean leading
`sudo CMD` or `doas CMD` to `pkexec CMD` so the escalation reaches the
supervised polkit prompt. Commands bearing options, environment assignments, or
shell metacharacters MUST NOT be rewritten — they exec the real binary unchanged.
The declared reason MUST NOT contain the command argv or any user-supplied
string.

#### Scenario: Clean sudo rewrite

- **WHEN** an agent process resolves `sudo pacman -Syu` through the shim
- **THEN** the shim declares intent (`channel: "shim"`) and execs
  `pkexec pacman -Syu`, producing a supervised, attributed bb-auth prompt

#### Scenario: Option-bearing sudo passes through

- **WHEN** an agent runs `sudo -u nobody id` through the shim
- **THEN** the command is not rewritten; the shim execs the real `sudo` and the
  escalation proceeds via sudo's normal path (unsupervised, as before)

#### Scenario: Reason is generic and secret-free

- **WHEN** the shim declares intent
- **THEN** the reason is a fixed generic string (no argv, no environment data),
  and the invoked command is sent only in the audit-only `command` field which
  prompts never render

### Requirement: Passthrough never declares

When the shim passes a command through unchanged (human invocation, or an agent
command that cannot be rewritten — including `pkexec`, which already reaches the
daemon), it MUST NOT emit a declaration, so it cannot clobber a real reason
declared earlier by the `bb-auth-declare` CLI or a harness hook under the
intent store's latest-wins rule.

#### Scenario: pkexec passthrough preserves prior declaration

- **WHEN** an agent runs `bb-auth-declare --reason "cleanup" -- pkexec rm /x`
  and the `pkexec` shim intercepts the exec'd `pkexec`
- **THEN** the shim emits no declaration and the prompt shows the real reason
  "cleanup", not the generic shim reason

### Requirement: Fail-open on any error

The shim MUST fail open on every error path — daemon unreachable, detection
failure, real-binary resolution failure — by exec'ing the originally intended
binary with the original argv. A shim MUST never block, drop, or alter the
command semantics of an invocation it could not fully process.

#### Scenario: Daemon unavailable

- **WHEN** the bb-auth socket is unreachable while an agent runs `sudo x`
- **THEN** the shim execs the real `sudo x` unchanged — without a daemon there is
  no supervised prompt to route to, and breaking `sudo` is worse than an
  unsupervised escalation; the command is never dropped

### Requirement: Opt-in activation

Shim binaries SHALL be installed to a dedicated directory (not the default
PATH), inert until the user prepends that directory to `PATH`. The package MUST
NOT alter the user's `PATH` automatically.

#### Scenario: Install is inert

- **WHEN** the package is installed or upgraded
- **THEN** no PATH modification occurs and `sudo` resolves to the system binary
  exactly as before

#### Scenario: Documented activation

- **WHEN** a user runs `bb-auth-declare --print-shim-dir`
- **THEN** the absolute shim directory path is printed for use in shell or
  session environment configuration
