#!/usr/bin/env python3
"""Tests for the pi extension (integrations/pi/bb-auth-extension.ts).

Drives the extension through a tiny node harness: registers a fake `pi` object
capturing the `tool_call` handler, then invokes it with bash-tool events against
a stub daemon socket. Skipped when node is unavailable.
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "fixtures", "escalation-translation.json")
EXT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "integrations", "pi", "bb-auth-extension.ts")

# Runs the extension's tool_call handler on argv[2] and prints the resulting
# event.input as JSON. Emulates pi's wiring: register handler, fire event.
DRIVER = """
const mod = await import(process.argv[2]);
const pi = { on: (n, f) => { pi._h = f; } };
await (mod.default ?? mod)(pi);
const event = { toolName: process.argv[3] || "bash", input: { command: process.argv[4] } };
await pi._h(event, { cwd: "/tmp", sessionManager: { getBranch: () => [] } });
console.log(JSON.stringify(event.input));
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


class PiExtensionCase(unittest.TestCase):
    def setUp(self):
        if not shutil.which("node"):
            self.skipTest("node not available")
        self.tmp = tempfile.TemporaryDirectory()
        self.runenv = os.path.join(self.tmp.name, "run")
        os.makedirs(self.runenv)
        self.sockpath = os.path.join(self.runenv, "bb-auth.sock")
        self.daemon = FakeDaemon(self.sockpath)
        self.daemon.start()
        driver = os.path.join(self.tmp.name, "driver.mjs")
        with open(driver, "w") as f:
            f.write(DRIVER)
        self.cmd = ["node", driver, EXT]
        self.env = {**os.environ, "XDG_RUNTIME_DIR": self.runenv}

    def tearDown(self):
        self.daemon.stop()
        self.tmp.cleanup()

    def invoke(self, command: str, tool: str = "bash"):
        return subprocess.run(self.cmd + [tool, command], env=self.env,
                              capture_output=True, text=True, timeout=15)

    def test_sudo_rewrites_and_declares(self):
        proc = self.invoke("sudo pacman -Sc")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["command"], "pkexec pacman -Sc")
        self.assertEqual(len(self.daemon.payloads), 1)
        d = self.daemon.payloads[0]
        self.assertEqual(d["agent"], "pi")
        self.assertEqual(d["channel"], "hook")

    def test_sudo_n_rewrites(self):
        proc = self.invoke("sudo -n make install")
        self.assertEqual(json.loads(proc.stdout)["command"], "pkexec make install")

    def test_sudo_user_flag_maps(self):
        proc = self.invoke("sudo -u nobody id")
        self.assertEqual(json.loads(proc.stdout)["command"], "pkexec --user nobody id")

    def test_unsupported_option_declares_but_no_rewrite(self):
        proc = self.invoke("sudo -E id")
        self.assertEqual(json.loads(proc.stdout)["command"], "sudo -E id")
        self.assertEqual(len(self.daemon.payloads), 1)

    def test_non_bash_tool_untouched(self):
        proc = self.invoke("sudo x", tool="edit")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["command"], "sudo x")
        self.assertEqual(self.daemon.payloads, [])

    def test_non_privileged_untouched(self):
        proc = self.invoke("ls -la")
        self.assertEqual(json.loads(proc.stdout)["command"], "ls -la")
        self.assertEqual(self.daemon.payloads, [])

    def test_metachar_declares_but_no_rewrite(self):
        proc = self.invoke("sudo apt update && sudo apt upgrade")
        self.assertEqual(json.loads(proc.stdout)["command"],
                         "sudo apt update && sudo apt upgrade")
        self.assertEqual(len(self.daemon.payloads), 1)

    def test_shared_translation_vectors(self):
        with open(FIXTURE) as f:
            rows = json.load(f)["rows"]
        for row in rows:
            with self.subTest(command=row["command"]):
                got = json.loads(self.invoke(row["command"]).stdout)["command"]
                self.assertEqual(got, row["rewrite"] or row["command"])

    def test_daemon_down_no_rewrite(self):
        self.daemon.stop()
        os.unlink(self.sockpath)
        proc = self.invoke("sudo true")
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(json.loads(proc.stdout)["command"], "sudo true")


if __name__ == "__main__":
    unittest.main()
