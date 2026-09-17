// bb-auth intent extension for pi — declare + sudo→pkexec rewrite on the
// `tool_call` event for the `bash` tool. Display/audit only, fail-open.
//
// Install: copy or symlink into ~/.pi/agent/extensions/ (global) or
// .pi/extensions/ (project), then `/reload`. The PATH shim (bb-auth-declare)
// remains the fallback for command shapes this hook misses.

import { connect } from "node:net"

// A privileged *leading* token only — `cat sudo.conf` must not declare: a stray
// "(no rationale captured)" would clobber a real pending reason (latest-wins).
const PRIVILEGED = /^\s*(sudo|pkexec|doas)\b/
const TTL_MS = 20000
const MAX_REASON = 2000

// Same conservative rule as the C++ hook / python shim: only a clean leading
// `sudo CMD` / `doas CMD` — no options, env assignments, or shell metachars.
function rewriteSudo(command) {
  const trimmed = command.trim()
  if (!trimmed.startsWith("sudo ") && !trimmed.startsWith("doas ")) return null
  if (/[|&;<>`$()\n]/.test(trimmed)) return null
  const rest = trimmed.slice(trimmed.indexOf(" ")).trim()
  if (!rest || rest.startsWith("-")) return null
  if (/^[^ ]*=[^ ]*/.test(rest.split(" ")[0])) return null
  return "pkexec " + rest
}

// Declare intent over the bb-auth socket and resolve true only when the daemon
// answers {"type":"ok"} — proof the supervised path is live. Bounded wait: a
// same-UID squatter that accepts-but-never-replies must not hang the tool call.
function declareIntent(reason, command, cwd) {
  return new Promise((resolve) => {
    const sockPath = `${process.env.XDG_RUNTIME_DIR || "/run/user/" + process.getuid()}/bb-auth.sock`
    const sock = connect(sockPath)
    let done = false
    const finish = (ok) => {
      if (!done) {
        done = true
        sock.destroy()
        resolve(ok)
      }
    }
    sock.setTimeout(500)
    sock.on("timeout", () => finish(false))
    sock.on("error", () => finish(false))
    sock.on("connect", () => {
      sock.write(
        JSON.stringify({
          type: "intent.declare",
          reason,
          agent: "pi",
          command,
          cwd: cwd || "",
          channel: "hook",
          ttlMs: TTL_MS,
        }) + "\n"
      )
    })
    sock.on("data", (d) => {
      try {
        finish(JSON.parse(d.toString().split("\n")[0]).type === "ok")
      } catch {
        finish(false)
      }
    })
  })
}

// Most recent assistant prose is the best guess at why the command runs —
// pi's sessionManager is current through the tool-calling message when
// `tool_call` fires. Any failure yields "" and the generic reason is used.
function lastAssistantText(ctx) {
  try {
    const branch = ctx?.sessionManager?.getBranch?.() ?? []
    for (let i = branch.length - 1; i >= 0; i--) {
      const e = branch[i]
      if (e?.type !== "message" || e.message?.role !== "assistant") continue
      const content = e.message.content
      if (typeof content === "string" && content.trim())
        return content.trim().slice(0, MAX_REASON)
      if (Array.isArray(content)) {
        const text = content
          .filter((b) => b?.type === "text" && typeof b.text === "string" && b.text.trim())
          .map((b) => b.text.trim())
          .join(" ")
        if (text) return text.slice(0, MAX_REASON)
      }
    }
  } catch {
    // session shape varies by version — fall through to the generic reason
  }
  return ""
}

export default function (pi) {
  pi.on("tool_call", async (event, ctx) => {
    try {
      if (event.toolName !== "bash") return
      const command = event.input?.command
      if (typeof command !== "string" || !PRIVILEGED.test(command)) return

      const rewritten = rewriteSudo(command)
      // Rewrite only when the daemon acknowledged: with no supervised agent,
      // pkexec can hard-fail where sudo would have worked — keep the command on
      // its original auth path instead (fail-open, same contract as the C++ hook).
      const daemonAnswered = await declareIntent(
        lastAssistantText(ctx) || "(no rationale captured)",
        rewritten || command,
        ctx?.cwd
      )
      if (rewritten && daemonAnswered) event.input.command = rewritten
    } catch {
      // fail-open: never block or alter a tool call on extension error
    }
  })
}
