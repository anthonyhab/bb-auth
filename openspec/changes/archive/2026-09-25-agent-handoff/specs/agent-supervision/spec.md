# Spec Delta

## ADDED Requirements

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
