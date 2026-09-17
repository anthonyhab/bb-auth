# Proposal

## Why

Agent intent attribution today depends on voluntary cooperation: an agent must read
the `bb-auth` skill and wrap its command in `bb-auth-declare`, or run under the one
harness with a compiled hook (Claude Code). An agent that doesn't cooperate runs
`sudo` straight through PAM — bb-auth never prompts at all, so the escalation is
unsupervised and unattributed. Reliability therefore varies by which agent happens
to be driving, which is exactly the failure this feature exists to prevent.

The fix is to move enforcement from the agent (voluntary) to the environment
(deterministic): `sudo`/`doas`/`pkexec` PATH shims that activate only under a
recognized agent ancestor and pass through untouched for humans.

## What Changes

- `bb-auth-declare` gains a **shim mode** selected by `argv[0]`: installed under
  `libexec/bb-auth/shims/{sudo,doas,pkexec}` as symlinks, it detects agent
  ancestry via the existing `/proc` walk.
  - Under a recognized agent: declares a generic intent (`channel: "shim"`) and,
    for a clean leading `sudo CMD`/`doas CMD`, rewrites to `pkexec CMD` so the
    escalation reaches bb-auth's supervised prompt. Anything else execs the real
    binary unchanged.
  - Under a human (no agent ancestor): transparent passthrough — the shim
    resolves the real binary by scanning `PATH` past itself and execs argv
    verbatim. Zero behavior change, zero overhead beyond one exec.
  - Declares **only when it rewrites**: a `pkexec` passthrough never emits a
    generic declare, so it cannot clobber a real reason declared moments earlier
    via `bb-auth-declare`/hook (IntentStore is latest-wins per agent).
- Fails open identically to the existing channels: daemon unreachable, detection
  failure, or resolution failure → exec the real binary unchanged.
- Shim dir installs inertly; activation is opt-in via documented PATH prepend
  (`bb-auth-declare --print-shim-dir` emits the path for shell config snippets).
- Docs: README/TROUBLESHOOTING gain shim setup + residual-limit notes; lat.md
  gains a `agent-intent#PATH shim channel` section.

## Capabilities

### New Capabilities

- `agent-supervision`: environmental enforcement of intent declaration and
  `sudo`/`doas`→`pkexec` routing for agent-initiated escalations, including
  agent-ancestry gating, human passthrough, rewrite rules, and fail-open
  behavior. (First capability spec in the repo.)

### Modified Capabilities

(none — no existing specs)

## Impact

- `integrations/agent-cli/bb-auth-declare` — argv0 shim mode, agent-detection
  split (detect vs. fallback), `--print-shim-dir`.
- `CMakeLists.txt` — install shim symlinks under `libexec/bb-auth/shims/`.
- `docs/` README/TROUBLESHOOTING + `lat.md/agent-intent.md` — new channel,
  residual limits (absolute-path `sudo`, non-PATH invocations).
- Provider contract: **unchanged** — reuses `intent.declare` with a new
  `channel` value (`"shim"`), which is a free-form string field.
- Trust boundary: unchanged — display/audit only, never gates allow/deny.
