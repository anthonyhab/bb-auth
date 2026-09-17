#!/usr/bin/env python3
"""Tests for bb-auth-declare shim mode (PATH interception of sudo/doas/pkexec).

Runs the real script through symlinks named sudo/doas/pkexec under a staged
PATH, with stub "real" binaries logging their argv and a stub daemon socket
recording declarations. The agent-ancestor path is exercised via a parent
process whose cmdline contains a known agent signature.
"""
from __future__ import annotations

import importlib.util
from importlib.machinery import SourceFileLoader
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.environ.get(
    "BB_AUTH_DECLARE",
    os.path.join(_HERE, "..", "integrations", "agent-cli", "bb-auth-declare.in"),
)


def _load_module():
    loader = SourceFileLoader("bb_auth_declare", SCRIPT)
    spec = importlib.util.spec_from_file_location("bb_auth_declare", SCRIPT, loader=loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def setUpModule():
    # configure_file does not preserve the exec bit in the build tree; the
    # installed copy gets it via install(PROGRAMS). Stage an executable copy
    # instead of mutating the source file's mode.
    global SCRIPT
    if not os.access(SCRIPT, os.X_OK):
        staged = os.path.join(tempfile.mkdtemp(prefix="bb-auth-declare-"), "bb-auth-declare")
        shutil.copyfile(SCRIPT, staged)
        os.chmod(staged, 0o755)
        SCRIPT = staged


class FakeDaemon(threading.Thread):
    """Minimal intent.declare endpoint: records payloads, answers bound=true."""

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


class ShimCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        self.shimdir = os.path.join(root, "shims")
        self.realdir = os.path.join(root, "real")
        self.fakedir = os.path.join(root, "fakeagent")
        self.runenv = os.path.join(root, "run")
        self.logfile = os.path.join(root, "execs.log")
        self.sockpath = os.path.join(self.runenv, "bb-auth.sock")
        for d in (self.shimdir, self.realdir, self.fakedir, self.runenv):
            os.makedirs(d)

        for tool in ("sudo", "doas", "pkexec"):
            os.symlink(SCRIPT, os.path.join(self.shimdir, tool))
            stub = os.path.join(self.realdir, tool)
            with open(stub, "w") as f:
                f.write('#!/bin/sh\necho "%s $@" >> "$BB_TEST_LOG"\n' % tool)
            os.chmod(stub, 0o755)

        # Fake agent parent: stays alive as the command's parent (no exec), so
        # the shim's ancestry walk sees a cmdline containing the signature.
        agent = os.path.join(self.fakedir, "opencode")
        with open(agent, "w") as f:
            f.write('#!/bin/sh\n"$@"\n')
        os.chmod(agent, 0o755)

        self.env = {
            "PATH": os.pathsep.join([self.shimdir, self.realdir, "/usr/bin", "/bin"]),
            "XDG_RUNTIME_DIR": self.runenv,
            "BB_TEST_LOG": self.logfile,
            "HOME": root,
        }
        self.daemon = None

    def tearDown(self):
        if self.daemon:
            self.daemon.stop()
        self.tmp.cleanup()

    def start_daemon(self):
        self.daemon = FakeDaemon(self.sockpath)
        self.daemon.start()

    def run_cmd(self, argv, via_agent=False):
        cmd = [os.path.join(self.fakedir, "opencode")] + argv if via_agent else argv
        return subprocess.run(cmd, env=self.env, timeout=15, capture_output=True, text=True)

    def exec_log(self):
        if not os.path.exists(self.logfile):
            return ""
        with open(self.logfile) as f:
            return f.read()

    def run_cmd_human(self, argv):
        """Run the shim with agent detection stubbed off — deterministic human
        passthrough even when the ambient test process sits under an agent
        (e.g. agent-driven CI)."""
        stub = (
            "import importlib.util,os,sys;"
            "from importlib.machinery import SourceFileLoader;"
            "p=os.environ['BB_AUTH_DECLARE'];"
            "s=importlib.util.spec_from_file_location('m',p,loader=SourceFileLoader('m',p));"
            "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
            "m._detect_agent=lambda:None;"
            "m._shim_mode(sys.argv[1],sys.argv[2:])"
        )
        return subprocess.run(
            [sys.executable, "-c", stub] + argv,
            env={**self.env, "BB_AUTH_DECLARE": SCRIPT},
            timeout=15, capture_output=True, text=True)

    # --- shim behavior ---

    def test_human_sudo_passthrough(self):
        self.start_daemon()
        self.run_cmd_human(["sudo", "pacman", "-Syu"])
        self.assertIn("sudo pacman -Syu", self.exec_log())
        self.assertNotIn("pkexec", self.exec_log())
        self.assertEqual(self.daemon.payloads, [])

    def test_agent_sudo_rewrites_and_declares(self):
        self.start_daemon()
        self.run_cmd(["sudo", "clean", "cache"], via_agent=True)
        self.assertIn("pkexec clean cache", self.exec_log())
        self.assertNotIn("sudo clean cache", self.exec_log())
        self.assertEqual(len(self.daemon.payloads), 1)
        p = self.daemon.payloads[0]
        self.assertEqual(p["type"], "intent.declare")
        self.assertEqual(p["channel"], "shim")
        self.assertEqual(p["command"], "sudo clean cache")
        self.assertNotIn("sudo", p["reason"])

    def test_agent_sudo_with_options_passes_through(self):
        self.start_daemon()
        self.run_cmd(["sudo", "-u", "nobody", "id"], via_agent=True)
        self.assertIn("sudo -u nobody id", self.exec_log())
        self.assertNotIn("pkexec", self.exec_log())
        self.assertEqual(self.daemon.payloads, [])

    def test_agent_pkexec_passthrough_never_declares(self):
        self.start_daemon()
        self.run_cmd(["pkexec", "rm", "/x"], via_agent=True)
        self.assertIn("pkexec rm /x", self.exec_log())
        self.assertEqual(self.daemon.payloads, [])

    def test_agent_doas_rewrites(self):
        self.start_daemon()
        self.run_cmd(["doas", "ls", "/root"], via_agent=True)
        self.assertIn("pkexec ls /root", self.exec_log())
        self.assertEqual(len(self.daemon.payloads), 1)
        self.assertEqual(self.daemon.payloads[0]["channel"], "shim")

    def test_daemon_down_falls_back_to_real_sudo(self):
        # No daemon listening: shim must not strand the command on pkexec.
        self.run_cmd(["sudo", "true"], via_agent=True)
        self.assertIn("sudo true", self.exec_log())
        self.assertNotIn("pkexec", self.exec_log())

    # --- unit-level ---

    def test_shim_rewrite_rules(self):
        m = _load_module()
        self.assertEqual(m._shim_rewrite("sudo", ["pacman", "-Syu"]), ["pkexec", "pacman", "-Syu"])
        self.assertEqual(m._shim_rewrite("doas", ["id"]), ["pkexec", "id"])
        self.assertIsNone(m._shim_rewrite("sudo", ["-u", "nobody", "id"]))
        self.assertIsNone(m._shim_rewrite("sudo", ["FOO=1", "id"]))
        self.assertIsNone(m._shim_rewrite("sudo", ["--", "id"]))
        self.assertIsNone(m._shim_rewrite("sudo", []))
        self.assertIsNone(m._shim_rewrite("pkexec", ["id"]))
        # Shell metacharacters in ANY argument must decline — parity with the
        # harness hook's refusal class.
        self.assertIsNone(m._shim_rewrite("sudo", ["sh", "-c", "a|b"]))
        self.assertIsNone(m._shim_rewrite("sudo", ["a|b"]))
        self.assertIsNone(m._shim_rewrite("sudo", ["x", "&&", "y"]))
        self.assertIsNone(m._shim_rewrite("sudo", ["$(whoami)"]))
        self.assertIsNone(m._shim_rewrite("sudo", ["cmd", "`id`"]))
        self.assertIsNone(m._shim_rewrite("sudo", ["cmd", ">out"]))
        self.assertEqual(m._rewrite_sudo(["sudo", "pacman", "-Syu"]), ["pkexec", "pacman", "-Syu"])
        self.assertEqual(m._rewrite_sudo(["sudo", "sh", "-c", "a;b"]), ["sudo", "sh", "-c", "a;b"])

    def test_agent_token_matching(self):
        m = _load_module()
        # Exact basenames, path segments, and script-entry forms match.
        self.assertEqual(m._agent_from_token("/opt/claude-code/cli.js"), "claude-code")
        self.assertEqual(m._agent_from_token("/usr/lib/node_modules/@google/gemini-cli/x.js"), "gemini-cli")
        self.assertEqual(m._agent_from_token("agy"), "gemini-cli")
        self.assertEqual(m._agent_from_token("codex.js"), "codex")
        # pi matches on its npm package dir, never the bare `pi` binary name.
        self.assertEqual(m._agent_from_token(
            "/usr/lib/node_modules/@earendil-works/pi-coding-agent/dist/cli.js"), "pi")
        self.assertIsNone(m._agent_from_token("pi"))
        self.assertIsNone(m._agent_from_token("/usr/bin/pi"))
        # Lookalikes must not match.
        self.assertIsNone(m._agent_from_token("node"))
        self.assertIsNone(m._agent_from_token("devin-notes.md"))
        self.assertIsNone(m._agent_from_token("strategy"))
        self.assertIsNone(m._agent_from_token("agyx"))

    def test_real_binary_skips_self(self):
        m = _load_module()
        env_path = os.pathsep.join([self.shimdir, self.realdir])
        old = os.environ.get("PATH")
        os.environ["PATH"] = env_path
        try:
            real = m._real_binary("sudo")
            self.assertIsNotNone(real)
            self.assertEqual(os.path.dirname(os.path.abspath(real)), self.realdir)
        finally:
            if old is None:
                del os.environ["PATH"]
            else:
                os.environ["PATH"] = old

    def test_print_shim_dir(self):
        out = subprocess.run([SCRIPT, "--print-shim-dir"], capture_output=True, text=True, timeout=15)
        self.assertEqual(out.returncode, 0)
        self.assertTrue(out.stdout.strip().endswith("bb-auth-shims"))


if __name__ == "__main__":
    unittest.main()
