// bb-auth intent plugin for opencode — declare + sudo→pkexec rewrite on the
// `bash` tool's tool.execute.before hook. Display/audit only, fail-open.
//
// Install: copy or symlink into ~/.config/opencode/plugins/ (global) or
// .opencode/plugins/ (project). The PATH shim (bb-auth-declare) remains the
// fallback for command shapes this hook misses.
//
// Note: some opencode versions had a bug where output.args mutations did not
// propagate to execution (anomalyco/opencode#31680). The declare still lands
// either way; on affected versions the PATH shim covers the actual exec.

import { connect } from "node:net"

// A privileged *leading* token only — `cat sudo.conf` must not declare: a stray
// "(no rationale captured)" would clobber a real pending reason (latest-wins).
const PRIVILEGED = /^\s*(sudo|pkexec|doas)\b/
const TTL_MS = 20000

// Same conservative rule as the C++ hook / python shim: only a clean leading
// `sudo CMD` — no options, env assignments, or shell metacharacters.
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
          agent: "opencode",
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

export const BbAuthIntent = async () => ({
  "tool.execute.before": async (input, output) => {
    if (input.tool !== "bash") return
    const command = output?.args?.command
    if (typeof command !== "string" || !PRIVILEGED.test(command)) return

    const rewritten = rewriteSudo(command)
    // Rewrite only when the daemon acknowledged: with no supervised agent,
    // pkexec can hard-fail where sudo would have worked — keep the command on
    // its original auth path instead (fail-open, same contract as the C++ hook).
    const daemonAnswered = await declareIntent(
      "(no rationale captured)",
      rewritten || command,
      output?.args?.workdir
    )
    if (rewritten && daemonAnswered) output.args.command = rewritten
  },
})
