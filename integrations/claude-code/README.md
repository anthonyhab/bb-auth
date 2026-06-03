# bb-auth agent intent integration

Make AI agents safer to run in auto-approve mode: when an agent runs a privileged
command (`sudo`/`pkexec`/`doas`), surface **who** is asking and **why** at the bb-auth
authentication prompt — and route the escalation through bb-auth's supervised prompt
instead of an unsupervised `sudo`.

`bb-auth-intent-hook.cpp` is a small Claude Code `PreToolUse` hook (a compiled
`Qt6::Core` console binary, installed to the daemon's `libexec`). It does two
display/audit-only things, both fail-open:

1. **Declares who + why.** It extracts the most recent assistant rationale from the
   transcript and POSTs `intent.declare` over the bb-auth socket. The daemon attributes
   the request from process ancestry and shows the reason at the prompt.
2. **Routes `sudo` → `pkexec`.** A *simple* leading `sudo CMD` is rewritten to
   `pkexec CMD` (via the PreToolUse `updatedInput` channel) so the escalation goes
   through bb-auth's polkit prompt — attributed and human-supervised — rather than
   sudo's PAM path, which bb-auth never sees.

## Why a compiled hook, gated

A `PreToolUse` matcher keys on the tool *name*, so a `Bash` hook would otherwise fire on
*every* command. Instead it is **`if`-gated** to privileged prefixes, so the process only
spawns when an agent is actually escalating privilege — zero overhead on ordinary Bash.
Being a native binary (no Python runtime), it starts in ~1 ms on that rare path and speaks
the socket directly.

## Trust model (important)

The declared reason and agent id are **self-asserted and display/audit only**. bb-auth
**never** uses them for the allow/deny decision — that stays with polkit and the daemon's
OS-resolved process identity (`docs/PROVIDER_CONTRACT.md` §3.1). The hook **fails open**:
malformed input, an unreachable daemon, or any error leaves the command unchanged and lets
it run. The `sudo`→`pkexec` rewrite is deliberately conservative — only a clean leading
`sudo CMD` with no options, env assignments, or shell metacharacters is rewritten;
anything compound or option-bearing runs unchanged (still with intent declared), because
pkexec's flags and minimal environment differ from sudo's.

The daemon correlates a declaration to the polkit request by **shared agent process
ancestry** (pid *and* start-time, so a recycled pid fails closed) — not by trusting the
declared `agent` field. If the declared agent disagrees with the resolved one, the prompt
shows a mismatch warning. The hook never sends the agent's command as authoritative: what
is actually authorized stays with polkit's own message.

A cross-agent MCP variant once lived here; it was cut to keep one good path. Recover it
from git history the day a non-Claude-Code agent needs explicit, agent-authored
declarations. See `docs/adr/0003-agent-intent-surfacing.md`.

## Setup (Claude Code)

The hook installs with the daemon to `${libexecdir}/bb-auth-intent-hook` (e.g.
`~/.local/libexec/bb-auth-intent-hook`). Add a `PreToolUse` hook on `Bash`, `if`-gated to
the privileged prefixes, in `~/.claude/settings.json`:

```json
{
  "hooks": {
    "PreToolUse": [
      { "matcher": "Bash",
        "hooks": [
          { "type": "command", "if": "Bash(sudo *)",   "command": "/home/you/.local/libexec/bb-auth-intent-hook" },
          { "type": "command", "if": "Bash(pkexec *)", "command": "/home/you/.local/libexec/bb-auth-intent-hook" },
          { "type": "command", "if": "Bash(doas *)",   "command": "/home/you/.local/libexec/bb-auth-intent-hook" }
        ] }
    ]
  }
}
```

Hooks are read at session start, so restart Claude Code after editing.

## Verify

In a fresh Claude Code session, ask it to run a `sudo` command (e.g. `sudo pacman -Syu`).
It should be rewritten to `pkexec` and surface the bb-auth prompt showing the agent
("claude-code", an AI-agent badge) and the declared reason. Run the same command directly
in a terminal (no agent) — it resolves normally with no reason, and the decision is
unaffected either way.
</content>
