#!/usr/bin/env python3
"""Tests for the opencode plugin (integrations/opencode/bb-auth-plugin.js).

Drives the plugin's `tool.execute.before` hook through a tiny node harness
against a stub daemon socket. Skipped when node is unavailable.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest

from test_pi_extension import FakeDaemon

_HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(_HERE, "fixtures", "escalation-translation.json")
PLUGIN = os.path.join(_HERE, "..", "integrations", "opencode", "bb-auth-plugin.js")

# Runs the plugin hook on argv[3] (tool) / argv[4] (command) and prints the
# resulting output.args as JSON.
DRIVER = """
const mod = await import(process.argv[2]);
const hooks = await mod.BbAuthIntent();
const output = { args: { command: process.argv[4], workdir: "/tmp" } };
await hooks["tool.execute.before"]({ tool: process.argv[3] }, output);
console.log(JSON.stringify(output.args));
"""


class OpencodePluginCase(unittest.TestCase):
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
        self.cmd = ["node", driver, os.path.abspath(PLUGIN)]
        self.env = {**os.environ, "XDG_RUNTIME_DIR": self.runenv}

    def tearDown(self):
        self.daemon.stop()
        self.tmp.cleanup()

    def invoke(self, command: str, tool: str = "bash"):
        proc = subprocess.run(self.cmd + [tool, command], env=self.env,
                              capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return json.loads(proc.stdout)["command"]

    def test_sudo_rewrites_and_declares(self):
        self.assertEqual(self.invoke("sudo pacman -Sc"), "pkexec pacman -Sc")
        self.assertEqual(self.daemon.payloads[0]["agent"], "opencode")

    def test_shared_translation_vectors(self):
        with open(FIXTURE) as f:
            rows = json.load(f)["rows"]
        for row in rows:
            with self.subTest(command=row["command"]):
                self.assertEqual(self.invoke(row["command"]), row["rewrite"] or row["command"])

    def test_non_bash_tool_untouched(self):
        self.assertEqual(self.invoke("sudo x", tool="edit"), "sudo x")
        self.assertEqual(self.daemon.payloads, [])

    def test_daemon_down_no_rewrite(self):
        self.daemon.stop()
        os.unlink(self.sockpath)
        self.assertEqual(self.invoke("sudo true"), "sudo true")


if __name__ == "__main__":
    unittest.main()
