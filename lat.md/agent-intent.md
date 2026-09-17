# Agent intent surfacing

Agent-initiated privileged commands get who+why attribution on the auth prompt: OS-resolved requestor identity plus a self-asserted declared reason — display/audit only, never part of the allow/deny path. (ADR 0003.)

## Why

An AI agent's `sudo`/`pkexec` escalation used to look identical to the user's own: the prompt answered *what* but not *who asked* or *why* — and `sudo` bypassed polkit/PAM entirely, never reaching bb-auth at all.

## Intent declaration and correlation

`intent.declare {reason, agent, command, channel}` binds to the declarer's OS-resolved ancestry, then attaches to the next polkit request as `Context.intent`.

- The binding key is the resolved agent identity ({pid, start-time} — a recycled pid fails closed), **not** the self-asserted `agent` field; when they disagree the daemon sets `mismatch = true` and the provider surfaces it.
- Implemented by `IntentStore` (`src/core/agent/IntentStore.hpp`); the requestor tag (`{name, isAgent, agentKind}`) is resolved by walking process ancestry — no agent cooperation needed.

## PreToolUse hook channel

`bb-auth-intent-hook` is a compiled `Qt6::Core` console binary installed to the daemon's libexec, spawned by a Claude Code `PreToolUse` hook — the zero-cooperation declaration channel.

- The matcher is **`if`-gated** (`if: "Bash(sudo *)"`/`pkexec`/`doas` permission-rule syntax) so the process spawns only on actual escalations: zero overhead on ordinary Bash, ~1 ms native startup on the rare privileged path. This is the load-bearing performance decision.
- An MCP variant was built and **deleted**: same declaration for a Claude-Code-only setup with more moving parts. Recover from git history if a non-Claude agent ever needs explicit declarations.
- Source: `integrations/claude-code/bb-auth-intent-hook.cpp`.

## Agent CLI channel

`bb-auth-declare` (`integrations/agent-cli/`) is the voluntary channel for harnesses without a hook: the agent runs `bb-auth-declare --reason … -- CMD`, which declares intent, rewrites a clean leading `sudo` to `pkexec`, and execs the command.

- Agent identity is auto-detected by walking PPID ancestry (`_detect_agent`), same signature family as the daemon's resolver.
- Fails open like the hook: declaration failure or an unbound (non-agent) declarer leaves the command unchanged — and the `sudo`→`pkexec` rewrite only happens when the declaration actually bound.

## sudo to pkexec rewrite

The hook rewrites a *clean leading* `sudo CMD` into `pkexec CMD` via the PreToolUse `updatedInput` channel, routing the escalation through bb-auth's supervised, attributed polkit prompt.

- Deliberately narrow: declines sudo options (`-i`, `-u`, …), env assignments, and shell metacharacters (`|`, `&&`, `;`, redirects, subshells, substitution) — pkexec's flags and minimal env differ from sudo's and mis-rewriting could change behaviour.
- Declined commands run unchanged, still with intent declared; the rewrite is surfaced via `permissionDecisionReason`, never silent.

## Attribution band

The production prompt renders, for agent requests only: agent glyph + resolved requestor name + "AI agent" badge, the declared reason, and a mismatch warning — a human request renders unchanged.

- The band **never** shows the declared `command`: correlation is by ancestry, not command match, so the string can diverge from what polkit actually authorizes — rendering it would manufacture false confidence.
- What is authorized stays with polkit's own message; the band lives in the prompt layer ([[architecture#Fallback UI]] `prompt/` extractors).

## Display-only trust boundary

Intent never gates the decision: a same-UID agent can declare a benign reason for a hostile command, so `reason`/`declaredAgent` carry zero authorization weight — allow/deny stays with polkit and OS-resolved identity.

The hook **fails open**: malformed input, unreachable daemon, or any error leaves the command unchanged.
