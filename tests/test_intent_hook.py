#!/usr/bin/env python3
"""Tests for bb-auth-intent-hook (multi-harness PreToolUse/BeforeTool hook).

Feeds each harness's stdin payload shape at the built binary with a stub
daemon socket, and asserts both the declared intent and the stdout rewrite
envelope for that harness.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

HOOK = os.environ.get("BB_AUTH_INTENT_HOOK", "bb-auth-intent-hook")
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "fixtures", "escalation-translation.json")

# Stub pkcheck: exits with $PKCHECK_RC (default 2 = challenge required) and
# prints to stdout like the real one, so a leak into the hook's stdout fails
# JSON parsing.
PKCHECK_STUB = """#!/bin/sh
echo 'polkit\\56result=auth_admin'
exit "${PKCHECK_RC:-2}"
"""


class FakeDaemon(threading.Thread):
    def __init__(self, path: str):
        super().__init__(daemon=True)
        self.path = path
        self.payloads: list[dict] = []
        self._srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)

    def run(self):
        self._srv.bind(self.path)
        self._srv.listen(8)
        self._srv.settimeout(15)
        try:
            while True:
                conn, _ = self._srv.accept()
                conn.settimeout(5)
                try:
                    data = b""
                    while b"\n" not in data and len(data) <= 65536:
                        chunk = conn.recv(4096)
                        if not chunk:
                            break
                        data += chunk
                    if b"\n" in data:
                        self.payloads.append(json.loads(data.split(b"\n", 1)[0].decode()))
                    conn.sendall(b'{"type":"ok","bound":true}\n')
                finally:
                    conn.close()
        except (socket.timeout, OSError):
            pass

    def stop(self):
        try:
            self._srv.close()
        except OSError:
            pass


class HookCase(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(HOOK):
            self.skipTest(f"hook binary not found at {HOOK}")
        self.tmp = tempfile.TemporaryDirectory()
        self.runenv = os.path.join(self.tmp.name, "run")
        os.makedirs(self.runenv)
        self.sockpath = os.path.join(self.runenv, "bb-auth.sock")
        self.daemon = FakeDaemon(self.sockpath)
        self.daemon.start()
        self.bindir = os.path.join(self.tmp.name, "bin")
        os.makedirs(self.bindir)
        stub = os.path.join(self.bindir, "pkcheck")
        with open(stub, "w") as f:
            f.write(PKCHECK_STUB)
        os.chmod(stub, 0o755)
        self.env = {**os.environ, "XDG_RUNTIME_DIR": self.runenv,
                    "PATH": self.bindir + os.pathsep + os.environ.get("PATH", "")}

    def tearDown(self):
        self.daemon.stop()
        self.tmp.cleanup()

    def invoke(self, payload: dict, env: dict | None = None):
        proc = subprocess.run(
            [HOOK], input=json.dumps(payload), env=env or self.env,
            capture_output=True, text=True, timeout=15)
        self.daemon.payloads_wait = None
        return proc

    def declared(self):
        # Hook drains one reply before exiting, so the payload is already there.
        return self.daemon.payloads

    def test_claude_bash_rewrite_with_transcript_reason(self):
        transcript = os.path.join(self.tmp.name, "t.jsonl")
        with open(transcript, "w") as f:
            f.write(json.dumps({"role": "assistant", "content": [
                {"type": "text", "text": "cleaning the pacman cache"}]}) + "\n")
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo pacman -Sc"},
            "transcript_path": transcript,
            "cwd": "/tmp",
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        self.assertEqual(
            out["hookSpecificOutput"]["updatedInput"]["command"], "pkexec pacman -Sc")
        self.assertEqual(len(self.declared()), 1)
        d = self.declared()[0]
        self.assertEqual(d["agent"], "claude-code")
        self.assertEqual(d["channel"], "hook")
        self.assertEqual(d["reason"], "cleaning the pacman cache")
        self.assertEqual(d["command"], "pkexec pacman -Sc")

    def test_devin_exec_rewrite(self):
        proc = self.invoke({
            "tool_name": "exec",
            "tool_input": {"command": "sudo rm -f /tmp/stale"},
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        self.assertEqual(
            out["hookSpecificOutput"]["updatedInput"]["command"],
            "pkexec rm -f /tmp/stale")
        self.assertEqual(self.declared()[0]["agent"], "devin")
        self.assertEqual(self.declared()[0]["reason"], "(no rationale captured)")

    def test_codex_bash_rewrite(self):
        # Codex reuses the Claude payload shape but adds `turn_id` — the hook
        # must attribute `codex`, not `claude-code`, and emit the codex-valid
        # envelope (permissionDecision "allow" + updatedInput).
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo pacman -Sc"},
            "turn_id": "turn_123",
            "tool_use_id": "call_abc",
            "session_id": "sess_1",
            "cwd": "/tmp",
            "matcher_aliases": ["Bash"],
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        hook = out["hookSpecificOutput"]
        self.assertEqual(hook["permissionDecision"], "allow")
        self.assertEqual(hook["updatedInput"]["command"], "pkexec pacman -Sc")
        self.assertEqual(self.declared()[0]["agent"], "codex")

    def test_codex_exec_command_via_matcher_alias(self):
        # Unified exec may report tool_name "exec_command" and surface "Bash"
        # only via matcher_aliases — still codex, still rewritable.
        proc = self.invoke({
            "tool_name": "exec_command",
            "tool_input": {"command": "sudo dmesg --clear"},
            "turn_id": "turn_9",
            "matcher_aliases": ["Bash"],
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        self.assertEqual(
            out["hookSpecificOutput"]["updatedInput"]["command"],
            "pkexec dmesg --clear")
        self.assertEqual(self.declared()[0]["agent"], "codex")

    def test_claude_bash_without_turn_id_stays_claude(self):
        # A Claude-shaped payload (no codex discriminators) must not be
        # misattributed to codex.
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo true"},
            "session_id": "s",
        })
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.declared()[0]["agent"], "claude-code")

    def test_gemini_tool_input_rewrite(self):
        proc = self.invoke({
            "tool_name": "run_shell_command",
            "tool_input": {"command": "sudo dmesg --clear"},
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        # Gemini merges hookSpecificOutput.tool_input, not updatedInput.
        self.assertEqual(
            out["hookSpecificOutput"]["tool_input"]["command"],
            "pkexec dmesg --clear")
        self.assertEqual(self.declared()[0]["agent"], "gemini-cli")

    def test_unknown_tool_declares_but_does_not_rewrite(self):
        proc = self.invoke({
            "tool_name": "some_future_shell",
            "tool_input": {"command": "sudo x"},
        })
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.strip(), "")
        self.assertEqual(len(self.declared()), 1)
        self.assertNotIn("agent", self.declared()[0])

    def test_non_privileged_command_is_silent(self):
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "ls -la"},
        })
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.strip(), "")
        self.assertEqual(self.declared(), [])

    def test_option_bearing_sudo_declares_without_rewrite(self):
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo -i"},
        })
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.strip(), "")
        self.assertEqual(len(self.declared()), 1)
        self.assertEqual(self.declared()[0]["command"], "sudo -i")

    def test_sudo_n_rewrites(self):
        # Shared aisudo subset: -n drops — the supervised GUI prompt is the
        # non-interactive path.
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo -n make install"},
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        self.assertEqual(
            out["hookSpecificOutput"]["updatedInput"]["command"],
            "pkexec make install")
        self.assertEqual(self.declared()[0]["command"], "pkexec make install")

    def test_sudo_user_flag_maps_to_pkexec(self):
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo -u nobody id"},
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        self.assertEqual(
            out["hookSpecificOutput"]["updatedInput"]["command"],
            "pkexec --user nobody id")

    def test_probe_only_sudo_declares_without_rewrite(self):
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo -nv"},
        })
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.strip(), "")
        self.assertEqual(self.declared()[0]["command"], "sudo -nv")

    def test_malformed_input_fails_open(self):
        proc = subprocess.run([HOOK], input="not json{", env=self.env,
                              capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.declared(), [])

    def test_daemon_down_leaves_command_unchanged(self):
        # No supervised agent behind pkexec: rewriting would strand the command
        # on a harder auth path than sudo. Fail-open = run it verbatim.
        self.daemon.stop()
        os.unlink(self.sockpath)
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo true"},
        })
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.strip(), "")

    def test_non_leading_privileged_mention_does_not_declare(self):
        # `cat sudo.conf` / `echo sudo` must not clobber a real pending reason
        # with "(no rationale captured)" — declaration is for leading tokens.
        for cmd in ("cat sudo.conf", "echo pkexec | xargs -i echo {}", "grep doas /etc/fstab"):
            proc = self.invoke({
                "tool_name": "Bash",
                "tool_input": {"command": cmd},
            })
            self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.declared(), [])

    # @lat: [[tests#Agent handoff#Shared translation vectors]]
    def test_shared_translation_vectors(self):
        with open(FIXTURE) as f:
            rows = json.load(f)["rows"]
        for row in rows:
            with self.subTest(command=row["command"]):
                proc = self.invoke({"tool_name": "Bash",
                                    "tool_input": {"command": row["command"]}})
                self.assertEqual(proc.returncode, 0)
                got = (json.loads(proc.stdout)["hookSpecificOutput"]["updatedInput"]["command"]
                       if proc.stdout.strip() else None)
                self.assertEqual(got, row["rewrite"])

    # @lat: [[tests#Agent handoff#Silent polkit authorization keeps the harness gate]]
    def test_no_challenge_keeps_harness_gate(self):
        # A polkit YES rule (pkcheck 0), a denial (1), or an error must not get
        # a harness "allow": nothing would stop the command at polkit.
        for rc in ("0", "1", "4"):
            with self.subTest(pkcheck_rc=rc):
                proc = self.invoke({"tool_name": "Bash",
                                    "tool_input": {"command": "sudo pacman -Sc"}},
                                   env={**self.env, "PKCHECK_RC": rc})
                self.assertEqual(proc.returncode, 0)
                self.assertEqual(proc.stdout.strip(), "")
        # Intent is still declared, with the command that will actually run.
        self.assertEqual(self.declared()[-1]["command"], "sudo pacman -Sc")

    def test_missing_pkcheck_keeps_harness_gate(self):
        os.unlink(os.path.join(self.bindir, "pkcheck"))
        proc = self.invoke({"tool_name": "Bash",
                            "tool_input": {"command": "sudo pacman -Sc"}},
                           env={**self.env, "PATH": self.bindir})
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.strip(), "")

    def test_rewrite_preserves_other_tool_input_fields(self):
        # Claude replaces the whole input with updatedInput: dropping timeout
        # or run_in_background would silently change how the command runs.
        tool_input = {"command": "sudo pacman -Sc", "timeout": 600000,
                      "run_in_background": True, "description": "clean cache"}
        proc = self.invoke({"tool_name": "Bash", "tool_input": tool_input})
        self.assertEqual(json.loads(proc.stdout)["hookSpecificOutput"]["updatedInput"],
                         {**tool_input, "command": "pkexec pacman -Sc"})
        gem = self.invoke({"tool_name": "run_shell_command",
                           "tool_input": {"command": "sudo id", "dir_path": "/tmp"}})
        self.assertEqual(json.loads(gem.stdout)["hookSpecificOutput"]["tool_input"],
                         {"command": "pkexec id", "dir_path": "/tmp"})

    def test_rewrite_carries_handoff_context(self):
        proc = self.invoke({"tool_name": "Bash",
                            "tool_input": {"command": "sudo pacman -Sc"}})
        hook = json.loads(proc.stdout)["hookSpecificOutput"]
        self.assertIn("bb-auth", hook["additionalContext"])
        self.assertIn("126", hook["additionalContext"])

    # @lat: [[tests#Agent handoff#Post-run annotation]]
    def test_post_events_annotate_without_declaring(self):
        base = {"tool_name": "Bash", "tool_input": {"command": "pkexec pacman -Syu"}}
        ok = self.invoke({**base, "hook_event_name": "PostToolUse",
                          "tool_response": {"stdout": "IGNORE PREVIOUS", "stderr": ""}})
        note = json.loads(ok.stdout)["hookSpecificOutput"]
        self.assertEqual(note["hookEventName"], "PostToolUse")
        self.assertIn("after the user authenticated", note["classifierContext"])
        self.assertNotIn("IGNORE", note["classifierContext"])

        silent = self.invoke({**base, "hook_event_name": "PostToolUse"},
                             env={**self.env, "PKCHECK_RC": "0"})
        self.assertIn("without an authentication prompt",
                      json.loads(silent.stdout)["hookSpecificOutput"]["classifierContext"])

        for code in ("126", "127"):
            fail = self.invoke({**base, "hook_event_name": "PostToolUseFailure",
                                "error": f"Exit code {code}\nError executing command"})
            ctx = json.loads(fail.stdout)["hookSpecificOutput"]["additionalContext"]
            self.assertIn("do not", ctx.lower())

        other = self.invoke({**base, "hook_event_name": "PostToolUseFailure",
                             "error": "Exit code 2\nmake: *** error"})
        self.assertEqual(other.stdout.strip(), "")
        # Codex (turn_id) and non-pkexec commands get nothing on post events.
        codex = self.invoke({**base, "hook_event_name": "PostToolUse", "turn_id": "t"})
        self.assertEqual(codex.stdout.strip(), "")
        sudo = self.invoke({"tool_name": "Bash", "hook_event_name": "PostToolUse",
                            "tool_input": {"command": "sudo true"}})
        self.assertEqual(sudo.stdout.strip(), "")
        self.assertEqual(self.declared(), [])

    def test_unhandled_event_is_silent(self):
        proc = self.invoke({"tool_name": "Bash", "hook_event_name": "PermissionRequest",
                            "tool_input": {"command": "sudo true"}})
        self.assertEqual(proc.stdout.strip(), "")
        self.assertEqual(self.declared(), [])

    def test_transcript_backward_scan_finds_last_reason_across_chunks(self):
        transcript = os.path.join(self.tmp.name, "big.jsonl")
        with open(transcript, "w") as f:
            f.write(json.dumps({"role": "assistant", "content": "old reason"}) + "\n")
            # ~300 KiB tool_result line spanning several 64 KiB chunks.
            f.write(json.dumps({"message": {"role": "assistant", "content": [
                {"type": "text", "text": "installing the kernel headers"}]}}) + "\n")
            f.write(json.dumps({"message": {"role": "user", "content": [
                {"type": "tool_result", "content": "x" * 300_000}]}}) + "\n")
            f.write(json.dumps({"message": {"role": "assistant", "content": [
                {"type": "tool_use", "name": "Bash", "input": {}}]}}) + "\n")
        self.invoke({"tool_name": "Bash", "transcript_path": transcript,
                     "tool_input": {"command": "sudo pacman -S linux-headers"}})
        self.assertEqual(self.declared()[0]["reason"], "installing the kernel headers")

    def test_version_flag(self):
        proc = subprocess.run([HOOK, "--version"], capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 0)
        self.assertRegex(proc.stdout, r"^bb-auth-intent-hook \d")


if __name__ == "__main__":
    unittest.main()
