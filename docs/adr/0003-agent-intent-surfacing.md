# 0003 — Surfacing agent-initiated privileged commands

Status: accepted (2026-06-03)

## Context

AI agents (Claude Code et al.) run privileged commands (`sudo`/`pkexec`/`doas`) on the user's
behalf, increasingly in auto-approve modes where the human supervises rather than typing each
command. When the OS auth prompt appears, it answers *what* is being authorized (polkit's
action message) but not *who* asked for it or *why*. A human at the prompt cannot tell an
agent-initiated escalation from one they started themselves. Worse, `sudo` does not use polkit
at all — it authenticates through PAM — so an agent's `sudo` never reaches bb-auth: no
attribution, no supervised prompt.

The daemon substrate for closing the attribution gap already existed (ADR 0001, 0002):

- It resolves **who** is asking by walking the requestor's process ancestry and tags
  `Context.requestor` with `{name, isAgent, agentKind, ...}` — no agent cooperation required.
- It accepts **`intent.declare`** `{reason, agent, command, channel}`, binds it to the
  declarer's **OS-resolved agent ancestry** (pid + start-time, so a recycled pid fails closed)
  — *not* the self-asserted `agent` field — and correlates it to the subsequent polkit request,
  attaching `Context.intent {reason, declaredAgent, channel, mismatch}`.
- `session.created` already serialises `requestor` and `intent` to the active provider.

What was open: (a) the declaration **channel**, (b) the **production prompt** ignored
`requestor`/`intent`, and (c) `sudo` escalations bypassed bb-auth entirely.

## Decision

**Channel: a Claude Code `PreToolUse` hook, compiled, and only that.** `bb-auth-intent-hook`
is a native `Qt6::Core` console binary installed to the daemon's `libexec`. A `PreToolUse`
matcher keys on the tool *name*, so a `Bash` hook would fire on every command; it is instead
**`if`-gated** (`if: "Bash(sudo *)"`, `pkexec`, `doas` — permission-rule syntax) so the process
**only spawns when an agent is actually escalating privilege**. That is the performance
decision: zero overhead on ordinary Bash, and on the rare privileged path a native binary
starts in ~1 ms and speaks the socket directly — no Python runtime. An MCP variant was built
and then **deleted**: for a Claude-Code-only setup it declares the same intent for the same
escalation with more moving parts. One good path; recover MCP from git history the day a
non-Claude agent needs explicit, agent-authored declarations.

**Route `sudo` → `pkexec` for the simple case, conservatively.** Because `sudo` bypasses
bb-auth, the hook rewrites a *clean leading* `sudo CMD` into `pkexec CMD` (via the PreToolUse
`updatedInput` channel) so the escalation routes through bb-auth's supervised, attributed
polkit prompt. The rewrite is deliberately narrow — it declines anything with sudo options
(`-i`, `-u`, ...), env assignments (`VAR=x cmd`), or shell metacharacters (`|`, `&&`, `;`,
redirects, subshells, command substitution), because pkexec's flag set and minimal environment
differ from sudo's and rewriting those could silently change behaviour. Declined commands run
unchanged, still with intent declared. The rewrite is surfaced to the agent
(`permissionDecisionReason`), not silent.

**The prompt renders who + why, never the self-asserted command.** The production prompt grows
an attribution band for agent requests only — agent glyph + requestor name + "AI agent" badge,
the declared reason, and a mismatch warning when `declaredAgent` disagrees with the resolved
agent. A human request renders unchanged. The band deliberately does **not** show the
agent-declared `command`: correlation is by process ancestry, not by command match, so the
declared command can diverge from the action polkit actually authorizes. Rendering it as fact
would manufacture false confidence. What is authorized stays with polkit's own message.

**Intent is display/audit only; it never gates the decision.** A same-UID agent can declare a
benign reason for a hostile command, so the reason and declared agent never enter the
allow/deny path — that stays with polkit and the daemon's OS-resolved identity. The hook
**fails open**: malformed input, an unreachable daemon, or any error leaves the command
unchanged. The `mismatch` flag is surfaced precisely so the human sees when the self-asserted
agent disagrees with the resolved one.

## Consequences

- Attribution appears on any agent escalation automatically (ancestry); the reason line and the
  `sudo`→`pkexec` routing appear when the hook ran. All degrade gracefully to the plain prompt.
- `sudo` escalations now prompt via bb-auth (no `NOPASSWD` fast-path for the rewritten case);
  that is the intended supervision, but it is a behaviour change for passwordless-sudo setups.
- The hook spawns only on privileged commands; a compound command that defeats the `if` parser
  may spawn the hook more than once (once per gate) — harmless, the declare is idempotent.
- `PROVIDER_CONTRACT` is unchanged — the band binds only to `requestor`/`intent` fields the
  contract already defines, and the rewrite is agent-side. No new IPC surface.
- The decision boundary is unmoved: this feature adds zero authorization weight.

## Alternatives considered

- **Python hook.** Simpler, but spawns a heavy interpreter; once `if`-gating removed the
  per-call cost, a native binary with no runtime dependency is the better fit for the repo.
- **No `if` gate (filter inside the hook).** The hook still self-filters, but the process would
  spawn on every Bash call. Rejected: the gate is a pure win.
- **MCP channel (kept alongside the hook).** Richer, cross-agent. Rejected for now: redundant
  for a Claude-Code-only setup and more to maintain. Deleted rather than carried dormant.
- **Aggressive rewrite (all `sudo`, including compound).** Maximises routing through bb-auth but
  risks mis-parsing and env-difference breakage. Rejected for the conservative simple-case rule.
- **Render the declared command in the band, or gate authz on `mismatch`.** Both let an
  unverified, self-asserted string influence trust or the decision. Rejected: who + why only,
  display-only.

## Residual limit

Perfect isolation between same-UID processes is not achievable without separate OS credentials
(same as ADR 0001). The reason and declared agent are self-asserted; the human, not the daemon,
judges the stated motive. This feature raises no new authorization surface.
</content>
