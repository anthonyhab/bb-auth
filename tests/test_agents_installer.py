#!/usr/bin/env python3
"""Tests for bb-auth-agents (harness installer).

Runs the binary against a throwaway HOME with an empty PATH so the real
machine's harness configs and binaries are never seen or touched.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
# The bb-auth-agent binary under its `bb-auth-agents` name (ctest passes the
# build-tree symlink).
SCRIPT = os.path.abspath(os.environ.get("BB_AUTH_AGENTS", "bb-auth-agents"))
INTEGRATIONS = os.path.normpath(os.path.join(_HERE, "..", "integrations"))
HOOK = "/opt/bb/libexec/bb-auth-intent-hook"

CLAUDE_BEFORE = {
    "permissions": {"allow": ["Bash(ls:*)"], "defaultMode": "auto"},
    "hooks": {
        "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "lat hook claude UserPromptSubmit"}]}],
        "PreToolUse": [{"matcher": "Edit", "hooks": [{"type": "command", "command": "fmt-check"}]}],
    },
    "autoMode": {"environment": ["**Trusted hosts**: debianbox"]},
}


class InstallerCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = self.tmp.name
        self.bindir = os.path.join(self.home, "bin")
        os.makedirs(self.bindir)
        self.env = {
            "HOME": self.home,
            "PATH": self.bindir,
            "XDG_RUNTIME_DIR": os.path.join(self.home, "run"),
            "BB_AUTH_INTENT_HOOK": HOOK,
            "BB_AUTH_INTEGRATIONS_DIR": INTEGRATIONS,
        }

    def tearDown(self):
        self.tmp.cleanup()

    def run_agents(self, *args):
        return subprocess.run([SCRIPT, *args], env=self.env,
                              capture_output=True, text=True, timeout=30)

    def write(self, rel, data):
        path = os.path.join(self.home, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            json.dump(data, f, indent=2)
            f.write("\n")
        return path

    def read(self, rel):
        with open(os.path.join(self.home, rel)) as f:
            return f.read()

    def fake_binary(self, name):
        path = os.path.join(self.bindir, name)
        with open(path, "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(path, 0o755)

    # @lat: [[tests#Agent handoff#Installer is idempotent and ownership-bounded]]
    def test_claude_install_idempotent_and_uninstall_restores(self):
        path = self.write(".claude/settings.json", CLAUDE_BEFORE)
        r = self.run_agents("install", "claude")
        self.assertEqual(r.returncode, 0, r.stderr)
        data = json.loads(self.read(".claude/settings.json"))
        pre = data["hooks"]["PreToolUse"]
        self.assertEqual(pre[0]["matcher"], "Edit")  # unrelated group kept first
        ours = pre[1]["hooks"]
        self.assertEqual([h["if"] for h in ours], ["Bash(sudo *)", "Bash(pkexec *)", "Bash(doas *)"])
        self.assertTrue(all(h["command"] == HOOK for h in ours))
        self.assertEqual(data["hooks"]["PostToolUse"][0]["hooks"][0]["if"], "Bash(pkexec *)")
        self.assertIn("PostToolUseFailure", data["hooks"])
        # Existing environment list is appended to as-is (no "$defaults"
        # injected); a created soft_deny list must keep the built-ins.
        env = data["autoMode"]["environment"]
        self.assertEqual(env[0], "**Trusted hosts**: debianbox")
        self.assertTrue(env[1].startswith("bb-auth:"))
        self.assertEqual(data["autoMode"]["soft_deny"][0], "$defaults")
        self.assertEqual(data["permissions"], CLAUDE_BEFORE["permissions"])
        self.assertTrue(os.path.exists(path + ".bb-auth-backup"))

        first = self.read(".claude/settings.json")
        r2 = self.run_agents("install", "claude")
        self.assertIn("unchanged", r2.stdout)
        self.assertEqual(self.read(".claude/settings.json"), first)
        self.assertIn("wired", self.run_agents("status").stdout.splitlines()[0])

        r3 = self.run_agents("uninstall", "claude")
        self.assertEqual(r3.returncode, 0, r3.stderr)
        self.assertEqual(json.loads(self.read(".claude/settings.json")), CLAUDE_BEFORE)

    def test_claude_creates_environment_with_defaults(self):
        self.write(".claude/settings.json", {})
        self.run_agents("install", "claude")
        data = json.loads(self.read(".claude/settings.json"))
        self.assertEqual(data["autoMode"]["environment"][0], "$defaults")
        self.run_agents("uninstall", "claude")
        self.assertEqual(json.loads(self.read(".claude/settings.json")), {})

    def test_user_entry_after_ours_does_not_cause_rewrites(self):
        self.write(".claude/settings.json", {})
        self.run_agents("install", "claude")
        data = json.loads(self.read(".claude/settings.json"))
        data["hooks"]["PreToolUse"].append({"matcher": "Write", "hooks": [{"type": "command", "command": "x"}]})
        self.write(".claude/settings.json", data)
        before = self.read(".claude/settings.json")
        self.assertIn("unchanged", self.run_agents("install", "claude").stdout)
        self.assertEqual(self.read(".claude/settings.json"), before)

    def test_stale_hook_path_is_replaced_not_duplicated(self):
        self.write(".claude/settings.json", {"hooks": {"PreToolUse": [{"matcher": "Bash", "hooks": [
            {"type": "command", "if": "Bash(sudo *)", "command": "/usr/libexec/bb-auth-intent-hook"}]}]}})
        self.assertIn("stale", self.run_agents("status").stdout.splitlines()[0])
        self.run_agents("install", "claude")
        data = json.loads(self.read(".claude/settings.json"))
        commands = [h["command"] for g in data["hooks"]["PreToolUse"] for h in g["hooks"]]
        self.assertEqual(commands, [HOOK] * 3)

    def test_codex_gemini_devin_shapes(self):
        self.write(".codex/hooks.json", {})
        self.write(".gemini/settings.json", {"theme": "x"})
        self.write(".config/devin/config.json", {"version": 1})
        r = self.run_agents("install", "codex", "gemini", "devin")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("/hooks", r.stdout)  # codex trust-review note
        codex = json.loads(self.read(".codex/hooks.json"))
        self.assertEqual(codex["hooks"]["PreToolUse"][0],
                         {"matcher": "Bash", "hooks": [{"type": "command", "command": HOOK}]})
        gemini = json.loads(self.read(".gemini/settings.json"))
        self.assertEqual(gemini["theme"], "x")
        self.assertEqual(gemini["hooks"]["BeforeTool"][0]["matcher"], "^run_shell_command$")
        devin = json.loads(self.read(".config/devin/config.json"))
        self.assertEqual(devin["hooks"]["PreToolUse"][0]["matcher"], "^exec$")

    def test_dropins_symlink_and_back_up_copies(self):
        plugdir = os.path.join(self.home, ".config/opencode/plugins")
        os.makedirs(plugdir)
        copied = os.path.join(plugdir, "bb-auth-plugin.js")
        with open(copied, "w") as f:
            f.write("// old hand-copied plugin\n")
        os.makedirs(os.path.join(self.home, ".pi/agent"))
        r = self.run_agents("install", "opencode", "pi")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.readlink(copied), os.path.join(INTEGRATIONS, "opencode", "bb-auth-plugin.js"))
        self.assertTrue(os.path.exists(copied + ".bb-auth-backup"))
        pi = os.path.join(self.home, ".pi/agent/extensions/bb-auth-extension.ts")
        self.assertTrue(os.path.islink(pi))
        self.assertIn("unchanged", self.run_agents("install", "pi").stdout)
        self.run_agents("uninstall", "opencode", "pi")
        self.assertFalse(os.path.lexists(copied))
        self.assertFalse(os.path.lexists(pi))

    def test_detection_targets_only_present_harnesses(self):
        self.assertEqual(self.run_agents("install").returncode, 1)  # nothing present
        self.fake_binary("claude")
        r = self.run_agents("install")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("claude", r.stdout)
        self.assertNotIn("codex", r.stdout)
        self.assertTrue(os.path.exists(os.path.join(self.home, ".claude/settings.json")))

    def test_dry_run_writes_nothing(self):
        path = self.write(".claude/settings.json", CLAUDE_BEFORE)
        before = self.read(".claude/settings.json")
        r = self.run_agents("install", "--dry-run", "claude")
        self.assertIn("would update", r.stdout)
        self.assertEqual(self.read(".claude/settings.json"), before)
        self.assertFalse(os.path.exists(path + ".bb-auth-backup"))

    def test_invalid_json_is_left_alone(self):
        path = os.path.join(self.home, ".claude/settings.json")
        os.makedirs(os.path.dirname(path))
        with open(path, "w") as f:
            f.write("{not json")
        r = self.run_agents("install", "claude")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.read(".claude/settings.json"), "{not json")

    def test_config_round_trips_byte_for_byte(self):
        # Order-preserving JSON: install + uninstall must give back the exact
        # bytes of a conventionally formatted file — key order, unicode,
        # escapes, number lexemes, and empty containers included.
        original = {
            "zeta": 1, "alpha": {"nested": [], "empty": {}, "list": [1.5e10, -0.25, 12345678901234567890]},
            "text": "caf\u00e9 \u2028 tab\tquote\" backslash\\ ctrl\u0001 emoji \U0001F512",
            "flags": [True, False, None],
            "hooks": {"PreToolUse": [{"matcher": "Edit", "hooks": [{"type": "command", "command": "x"}]}]},
        }
        path = os.path.join(self.home, ".claude/settings.json")
        os.makedirs(os.path.dirname(path))
        with open(path, "w") as f:
            f.write(json.dumps(original, indent=2, ensure_ascii=False) + "\n")
        with open(path, "rb") as f:
            before = f.read()
        self.assertEqual(self.run_agents("install", "claude").returncode, 0)
        data = json.loads(self.read(".claude/settings.json"))
        self.assertEqual(list(data)[:2], ["zeta", "alpha"])  # not re-sorted
        self.assertEqual(self.run_agents("uninstall", "claude").returncode, 0)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), before)

    def test_rejects_non_json_configs(self):
        for bad in ('{"a": 1,}', '{"a": 1} // comment', "[1, 2", '{"a": 01}', '{"a": "\\x"}'):
            with self.subTest(bad=bad):
                path = os.path.join(self.home, ".claude/settings.json")
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w") as f:
                    f.write(bad)
                self.assertEqual(self.run_agents("install", "claude").returncode, 1)
                self.assertEqual(self.read(".claude/settings.json"), bad)

    def test_status_reports_hook_daemon_polkit(self):
        out = self.run_agents("status").stdout
        self.assertIn("MISSING at " + HOOK, out)
        self.assertIn("NOT reachable", out)
        self.assertIn("pkcheck unavailable", out)


if __name__ == "__main__":
    unittest.main()
