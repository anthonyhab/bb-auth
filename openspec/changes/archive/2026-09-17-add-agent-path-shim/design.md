# Design

## Context

See proposal.md — Why. Relevant current state: `integrations/agent-cli/bb-auth-declare`
(Python 3, no deps beyond stdlib) already implements intent declaration over the
daemon socket, PPID-ancestry agent detection (`_detect_agent`), conservative
`sudo`→`pkexec` rewriting (`_rewrite_sudo`), and fail-open exec. The daemon binds
declarations to OS-resolved agent ancestry and correlates them latest-wins per
agent (`IntentStore`). `main.cpp` already uses argv0-dispatch for pinentry/keyring
personalities — shim mode follows the same convention.

## Goals / Non-Goals

**Goals:**

- Every agent-launched `sudo`/`doas`/`pkexec` that resolves via `PATH` gets bb-auth
  supervision with zero agent cooperation, once the shim dir is on `PATH`.
- Perfect transparency for humans: passthrough must be indistinguishable from the
  real binary except for one extra exec.
- Reuse, not parallel machinery: one script, one signature list, one socket client.

**Non-Goals:**

- Not an authorization boundary — display/audit only, same as all intent channels.
- No interception of absolute-path invocations (`/usr/bin/sudo`), shell builtins,
  or non-PATH execution — documented residual limit.
- No auto-PATH modification by the package; activation is user opt-in.
- No per-agent real reasons — the shim cannot know intent; hooks/CLI remain the
  quality path for reasons.

## Decisions

**argv0 dispatch inside `bb-auth-declare`, not a new binary.** The shim needs the
same detection, socket client, rewrite table, and fail-open behavior the script
already has. One install unit, one audit surface. Mirrors `main.cpp`'s
`detectModeFromArgv0`. Alternatives considered: a second compiled helper (needs a
C++ agent-signature duplicate — drift risk); tiny shell wrappers (still shells out
to the script, extra fork, no gain).

**PATH-scan passthrough for the real binary.** The shim resolves the real
`sudo`/`doas`/`pkexec` by walking `PATH` entries in order, skipping entries whose
resolved file is the shim itself (realpath compare). This respects system layout
(`/usr/bin` vs `/bin` vs `/usr/local/bin`) and survives reordered PATHs.
Alternative — hardcoded `/usr/bin/sudo`: wrong on NixOS, brew, merged-usr corner
cases; rejected. If resolution fails, exec via `execvp` of the *basename* —
PATH still contains the real binary further down, and the worst case is exec
failure, which is fail-open (exec error = command fails like normal ENOENT, never
silently altered). Wait — that would re-enter the shim infinitely. Corrected: on
resolution failure, exec the basename *with the shim dir removed from PATH* for
that exec call (set env accordingly); if still unresolved, exit 127 with stderr
note.

**Declare only when rewriting.** Latest-wins correlation means a generic shim
declare would clobber a real reason declared microseconds earlier by
`bb-auth-declare --reason X -- pkexec …` (the CLI execs pkexec → hits the pkexec
shim). So: shim declares iff it will rewrite `sudo`/`doas`→`pkexec`. `pkexec`
passthrough under an agent emits nothing — the prompt still shows agent
attribution via ancestry; only the generic reason is forgone. Alternative —
declare-always with reason-priority in IntentStore: adds cross-channel semantics
to the store for a cosmetic gain; rejected.

**`doas` rewrites like `sudo` under the simple-case rule.** The hook and CLI
decline to rewrite `doas` because its auth semantics differ from polkit's — but a
doas shim that passes through is an unsupervised hole, which is worse than a
semantic difference the user opted into by installing the shim. Clean `doas CMD`
→ `pkexec CMD`; option-bearing/compound → passthrough to real doas (residual
hole, documented). Alternative — declare-but-passthrough doas: declaration can
never correlate (no polkit request follows), so it's dead weight; rejected.

**Generic fixed reason, argv only in `command`.** Reason is rendered at the
prompt; per ADR-0003 the declared `command` is never rendered (self-asserted).
The shim's reason is a fixed string — `"Agent ran a privileged command without
declaring a reason"` — honest signal that the agent went through the shim, not a
cooperative channel. Argv goes to the audit-only `command` field.

**Install location `libexec/bb-auth/shims/`, symlinks to `bb-auth-declare`.**
Inert on install — nothing resolves through it until a user prepends it to PATH.
Symlinks are absolute (`<bindir>/bb-auth-declare`) computed at install time;
PKGBUILD/Nix flows already compute these prefixes. `bb-auth-declare
--print-shim-dir` prints the dir so setup docs/scripts stay machine-agnostic.

**Python is acceptable here.** `bb-auth-declare` already requires python3 in
BINDIR; the shim adds no new dependency. Per-invocation cost (~30-50 ms
interpreter start) is irrelevant on the privileged-command path and zero for
humans (passthrough still runs the script — hmm, see Risks).

## Risks / Trade-offs

- [Human `sudo` pays a Python startup (~30-50 ms) before passthrough] → Acceptable
  for the opt-in audience (users who enable the shim want agent supervision);
  documented. A compiled shim is the follow-up if this ever matters.
- [An agent could bypass the shim with `/usr/bin/sudo` or `env -i`] → Residual
  limit; the shim is supervision UX, not a security boundary — declared reasons
  and routing carry zero authorization weight either way.
- [Human shells nested under an agent process (e.g. a terminal spawned by the
  agent's UI) get intercepted] → Same false-positive the daemon's ancestry
  resolver already has; prompt still supervises correctly, attribution is
  defensible ("launched by agent"). Acceptable.
- [PATH-order fragility: another shim/dir earlier in PATH wins] → Documented;
  `--print-shim-dir` + setup snippet instruct prepend, not append.
- [Recursive re-exec if PATH-scan finds only the shim] → Resolution skips self by
  realpath; failure path execs with shim dir stripped from PATH. Covered by tests.

## Migration Plan

None required — additive, inert until opted in. Rollback: remove the shim dir
from PATH (instant) or uninstall the package.
