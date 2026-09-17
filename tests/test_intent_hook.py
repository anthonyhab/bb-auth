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
        self.env = {**os.environ, "XDG_RUNTIME_DIR": self.runenv}

    def tearDown(self):
        self.daemon.stop()
        self.tmp.cleanup()

    def invoke(self, payload: dict):
        proc = subprocess.run(
            [HOOK], input=json.dumps(payload), env=self.env,
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

    def test_malformed_input_fails_open(self):
        proc = subprocess.run([HOOK], input="not json{", env=self.env,
                              capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.declared(), [])

    def test_daemon_down_still_allows_and_rewrites(self):
        self.daemon.stop()
        os.unlink(self.sockpath)
        proc = self.invoke({
            "tool_name": "Bash",
            "tool_input": {"command": "sudo true"},
        })
        self.assertEqual(proc.returncode, 0)
        out = json.loads(proc.stdout)
        self.assertEqual(
            out["hookSpecificOutput"]["updatedInput"]["command"], "pkexec true")


if __name__ == "__main__":
    unittest.main()
