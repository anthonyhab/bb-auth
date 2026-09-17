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

import { spawn } from "node:child_process"

const PRIVILEGED = /\b(sudo|pkexec|doas)\b/

// Same conservative rule as the C++ hook / python shim: only a clean leading
// `sudo CMD` — no options, env assignments, or shell metacharacters.
function rewriteSudo(command) {
  const trimmed = command.trim()
  if (!trimmed.startsWith("sudo ")) return null
  if (/[|&;<>`$()\n]/.test(trimmed)) return null
  const rest = trimmed.slice(5).trim()
  if (!rest || rest.startsWith("-")) return null
  if (/^[^ ]*=[^ ]*/.test(rest.split(" ")[0])) return null
  return "pkexec " + rest
}

function declare(reason, command, cwd) {
  try {
    const child = spawn("bb-auth-declare", ["--reason", reason, "--agent", "opencode"], {
      stdio: "ignore",
      cwd: cwd || undefined,
    })
    child.on("error", () => {})
  } catch {}
}

export const BbAuthIntent = async () => ({
  "tool.execute.before": async (input, output) => {
    if (input.tool !== "bash") return
    const command = output?.args?.command
    if (typeof command !== "string" || !PRIVILEGED.test(command)) return

    const rewritten = rewriteSudo(command)
    declare("(no rationale captured)", rewritten || command, output?.args?.workdir)
    if (rewritten) output.args.command = rewritten
  },
})
