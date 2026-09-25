# agent-supervision Specification

## Purpose

Environmental supervision of agent-initiated privilege escalation: PATH shims that
declare intent and route `sudo`/`doas` through bb-auth's supervised prompt without
requiring any cooperation from the agent, while remaining transparent to humans.

## Requirements

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

### Requirement: Passthrough never declares

When the shim passes a command through unchanged (human invocation, or an agent
command that cannot be rewritten — including `pkexec`, which already reaches the
daemon), it MUST NOT emit a declaration, so it cannot clobber a real reason
declared earlier by the `aisudo` CLI or a harness hook under the
intent store's latest-wins rule.

#### Scenario: pkexec passthrough preserves prior declaration

- **WHEN** an agent runs `aisudo -r "cleanup" -- pkexec rm /x`
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

- **WHEN** a user runs `aisudo --print-shim-dir`
- **THEN** the absolute shim directory path is printed for use in shell or
  session environment configuration

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

### Requirement: Challenge-gated harness approval

The harness hook SHALL emit a `sudo`/`doas` → `pkexec` rewrite (and with it
any `permissionDecision: "allow"`) only when `pkcheck --action-id
org.freedesktop.policykit.exec --process <hook pid>`, run without user
interaction, exits 2 (authentication challenge required). Any other result —
authorized without challenge, not authorized, error, timeout (1 s), or
`pkcheck` unavailable — MUST leave the command unrewritten so the harness's own
permission path still applies. Intent is declared either way.

#### Scenario: Challenge required

- **WHEN** a Claude Code payload carries `sudo pacman -Sc`, the daemon answers,
  and `pkcheck` exits 2
- **THEN** the hook emits `updatedInput` `pkexec pacman -Sc` with
  `permissionDecision: "allow"`

#### Scenario: Silent authorization keeps the harness gate

- **WHEN** the same payload runs and `pkcheck` exits 0 (a polkit rule returns
  YES)
- **THEN** the hook emits no rewrite and no decision

#### Scenario: pkcheck missing

- **WHEN** `pkcheck` cannot be executed
- **THEN** the hook emits no rewrite

### Requirement: Handoff context for the model

A rewrite envelope SHALL include `additionalContext` stating that the command
was routed to a bb-auth GUI prompt the user approves, and that a dismissed or
failed prompt means the user declined.

#### Scenario: Rewrite tells the model

- **WHEN** the hook emits a rewrite for a Claude Code or Codex payload
- **THEN** `hookSpecificOutput.additionalContext` is non-empty and names bb-auth

### Requirement: Post-run annotation

For Claude Code payloads whose command starts with `pkexec`, the hook SHALL
handle `PostToolUse` by emitting `classifierContext` — "authorized by polkit
after the user authenticated" when a fresh `pkcheck` still exits 2, otherwise
"authorized by polkit without an authentication prompt" — and SHALL handle
`PostToolUseFailure` whose `error` begins `Exit code 126` or `Exit code 127` by
emitting `additionalContext` instructing the model not to circumvent the
decline. It MUST NOT copy tool output into either field and MUST NOT declare
intent on post events.

#### Scenario: Dismissed prompt

- **WHEN** a `PostToolUseFailure` payload has command `pkexec pacman -Syu` and
  error `Exit code 126\n...`
- **THEN** stdout carries `additionalContext` naming the decline, and no
  declaration reaches the daemon

#### Scenario: Unrelated failure ignored

- **WHEN** the failed command is `pkexec make` with `Exit code 2`
- **THEN** the hook prints nothing

### Requirement: Harness installer

`bb-auth-agents` SHALL provide `status`, `install [HARNESS...]`, and
`uninstall [HARNESS...]` for `claude`, `codex`, `gemini`, `devin`, `opencode`,
and `pi`. Install MUST be idempotent (re-running yields byte-identical files),
MUST back up a config file before its first modification, and MUST touch only
entries it owns (hook commands whose basename is `bb-auth-intent-hook`,
autoMode strings prefixed `bb-auth:`, symlinks pointing into bb-auth's datadir).
When it creates an `autoMode.environment` or `autoMode.soft_deny` array it MUST
include `"$defaults"`. With no harness arguments, install targets harnesses
that are present (binary on PATH or config file exists). `--dry-run` prints
the planned changes without writing.

#### Scenario: Idempotent Claude install

- **WHEN** `bb-auth-agents install claude` runs twice against a settings file
  with unrelated hooks and permissions
- **THEN** the second run changes nothing and unrelated keys are preserved

#### Scenario: Uninstall restores ownership boundary

- **WHEN** `bb-auth-agents uninstall claude` runs after install
- **THEN** no `bb-auth-intent-hook` command or `bb-auth:` autoMode entry
  remains and unrelated entries are unchanged

### Requirement: Shared translation vectors

Every sudo→pkexec translator (hook, aisudo, pi extension, opencode plugin)
SHALL be tested against the single fixture
`tests/fixtures/escalation-translation.json`.

#### Scenario: Translator drift fails tests

- **WHEN** any translator's output for a fixture row differs from the row's
  expected `rewrite`
- **THEN** that translator's test fails
