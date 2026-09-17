# Agent intent surfacing

Agent-initiated privileged commands get who+why attribution on the auth prompt: OS-resolved requestor identity plus a self-asserted declared reason — display/audit only, never part of the allow/deny path. (ADR 0003.)

## Why

An AI agent's `sudo`/`pkexec` escalation used to look identical to the user's own: the prompt answered *what* but not *who asked* or *why* — and `sudo` bypassed polkit/PAM entirely, never reaching bb-auth at all.

## Intent declaration and correlation

`intent.declare {reason, agent, command, channel}` binds to the declarer's OS-resolved ancestry, then attaches to the next polkit request as `Context.intent`.

- The binding key is the resolved agent identity ({pid, start-time} — a recycled pid fails closed), **not** the self-asserted `agent` field; when they disagree the daemon sets `mismatch = true` and the provider surfaces it.
- Correlation is per-agent-root, not per-command: a stale declaration can attach to a sibling's unrelated escalation — bounded by the 20 s TTL, consume-once, and the band never rendering the declared command.
- Implemented by `IntentStore` (`src/core/agent/IntentStore.hpp`); the requestor tag (`{name, isAgent, agentKind}`) is resolved by walking process ancestry — no agent cooperation needed.
- Agent recognition is **token-aware**: an argv token's path segment or basename must equal a known alias (script-entry forms like `codex.js`/`claude.exe` included) — raw substring matching was rejected because `vim devin-notes.md` or `agyx` must not impersonate an agent. Residual: `exec -a` self-labelling is display-only (a process can only mislabel *its own* prompts).

## Harness hook channel

`bb-auth-intent-hook` is a compiled `Qt6::Core` console binary spawned by a harness hook, covering Claude Code (`PreToolUse`), Devin, and Gemini CLI (`BeforeTool`) — a zero-cooperation declaration channel.

- Harness is auto-detected from the payload's `tool_name` (`Bash`/`exec`/`run_shell_command`); the rewrite envelope differs per harness (`updatedInput` merge vs Gemini's `hookSpecificOutput.tool_input`). Unknown tools still declare but never rewrite.
- The rewrite is **gated on a daemon `ok` reply** — an unreachable daemon leaves the command on its original auth path, since `pkexec` with no supervising agent can hard-fail where `sudo` would have worked.
- Declaration triggers only on a privileged *leading* token — `cat sudo.conf` must not clobber a real pending reason under latest-wins correlation.
- Claude Code's matcher is **`if`-gated** (`if: "Bash(sudo *)"`/`pkexec`/`doas` permission-rule syntax) so the process spawns only on actual escalations; Devin/Gemini matchers are tool-name regexes, so the hook self-filters privileged prefixes in ~1 ms. This is the load-bearing performance decision.
- A real rationale reaches the prompt only when the harness exposes `transcript_path` (Claude; Devin is Claude-format compatible); other harnesses declare `(no rationale captured)`.
- An MCP variant was built and **deleted**: same declaration for a Claude-Code-only setup with more moving parts. The shim now covers hookless agents environmentally.
- Source: `integrations/hooks/bb-auth-intent-hook.cpp`.

## Agent CLI channel

`bb-auth-declare` (`integrations/agent-cli/`) is the voluntary channel for harnesses without a hook: the agent runs `bb-auth-declare --reason … -- CMD`, which declares intent, rewrites a clean leading `sudo` to `pkexec`, and execs the command.

- Agent identity is auto-detected by walking PPID ancestry (`_detect_agent`), same signature family as the daemon's resolver.
- Fails open like the hook: declaration failure or an unbound (non-agent) declarer leaves the command unchanged — and the `sudo`→`pkexec` rewrite only happens when the declaration actually bound.

## PATH shim channel

`bb-auth-declare` doubles as `sudo`/`doas`/`pkexec` via argv0 dispatch, installed as symlinks under `<libexec>/bb-auth-shims/` — the environmental channel that needs no agent cooperation at all, just a PATH prepend.

- Activation gates on the same ancestry detection: under a recognized agent it declares a fixed generic reason (`channel="shim"`, argv only in the audit-only `command` field) and rewrites clean `sudo CMD`/`doas CMD` to `pkexec CMD`; under humans it execs the real binary verbatim (PATH scan skipping self).
- Passthrough never declares — latest-wins correlation means a passthrough declare would clobber a real reason set moments earlier by the CLI/hook.
- Daemon unreachable → passthrough rather than rewrite: a dead daemon would leave pkexec with no agent to authorize against, so keeping `sudo` on its own path beats forced supervision.
- Residual limits: absolute-path calls and non-PATH exec bypass it; it is supervision UX, not a security boundary.

## sudo to pkexec rewrite

The hook rewrites a *clean leading* `sudo CMD`/`doas CMD` into `pkexec CMD` via the harness's input-merge channel, routing the escalation through bb-auth's supervised, attributed polkit prompt.

- Deliberately narrow: declines sudo options (`-i`, `-u`, …), env assignments, and shell metacharacters (`|`, `&&`, `;`, redirects, subshells, substitution) — pkexec's flags and minimal env differ from sudo's and mis-rewriting could change behaviour.
- Declined commands run unchanged, still with intent declared; the rewrite is surfaced via `permissionDecisionReason`, never silent.

## Attribution band

The production prompt renders, for agent requests only: agent glyph + resolved requestor name + "AI agent" badge, the declared reason, and a mismatch warning — a human request renders unchanged.

- An agent request with **no declared reason** renders an explicit `No reason declared by the agent.` line — a missing reason reads as a signal, not an absence (`PromptDisplayModel::agentRequestor` drives the state).
- The band **never** shows the declared `command`: correlation is by ancestry, not command match, so the string can diverge from what polkit actually authorizes — rendering it would manufacture false confidence.
- What is authorized stays with polkit's own message; the band lives in the prompt layer ([[architecture#Fallback UI]] `prompt/` extractors).

## Display-only trust boundary

Intent never gates the decision: a same-UID agent can declare a benign reason for a hostile command, so `reason`/`declaredAgent` carry zero authorization weight — allow/deny stays with polkit and OS-resolved identity.

The hook **fails open**: malformed input, unreachable daemon, or any error leaves the command unchanged.
