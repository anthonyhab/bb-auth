#!/usr/bin/env python3
"""Tests for the aisudo CLI and shim mode (PATH interception of sudo/doas/pkexec).

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
    "AISUDO",
    os.path.join(_HERE, "..", "integrations", "agent-cli", "aisudo.in"),
)


def _load_module():
    loader = SourceFileLoader("aisudo", SCRIPT)
    spec = importlib.util.spec_from_file_location("aisudo", SCRIPT, loader=loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def setUpModule():
    # configure_file does not preserve the exec bit in the build tree; the
    # installed copy gets it via install(PROGRAMS). Stage an executable copy
    # instead of mutating the source file's mode.
    global SCRIPT
    if not os.access(SCRIPT, os.X_OK):
        staged = os.path.join(tempfile.mkdtemp(prefix="aisudo-"), "aisudo")
        shutil.copyfile(SCRIPT, staged)
        os.chmod(staged, 0o755)
        SCRIPT = staged


class FakeDaemon(threading.Thread):
    """Minimal intent.declare endpoint: records payloads, answers ok/bound."""

    def __init__(self, path: str, bound: bool = True):
        super().__init__(daemon=True)
        self.path = path
        self.bound = bound
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
                    conn.sendall(
                        b'{"type":"ok","bound":%s}\n'
                        % (b"true" if self.bound else b"false"))
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

    def start_daemon(self, bound=True):
        self.daemon = FakeDaemon(self.sockpath, bound=bound)
        self.daemon.start()

    def run_cli(self, argv, daemon=True, stdin_text=None):
        """Invoke the script by its real name (CLI mode) with a PATH whose
        stubs resolve directly — no shim re-entry."""
        if daemon and self.daemon is None:
            self.start_daemon()
        env = dict(self.env)
        env["PATH"] = os.pathsep.join([self.realdir, "/usr/bin", "/bin"])
        return subprocess.run([SCRIPT] + argv, env=env, timeout=15,
                              capture_output=True, text=True, input=stdin_text)

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
            "p=os.environ['AISUDO'];"
            "s=importlib.util.spec_from_file_location('m',p,loader=SourceFileLoader('m',p));"
            "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
            "m._detect_agent=lambda:None;"
            "m._shim_mode(sys.argv[1],sys.argv[2:])"
        )
        return subprocess.run(
            [sys.executable, "-c", stub] + argv,
            env={**self.env, "AISUDO": SCRIPT},
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

    def test_agent_sudo_u_maps_to_pkexec_user(self):
        self.start_daemon()
        self.run_cmd(["sudo", "-u", "nobody", "id"], via_agent=True)
        self.assertIn("pkexec --user nobody id", self.exec_log())
        self.assertNotIn("sudo -u nobody id", self.exec_log())
        self.assertEqual(len(self.daemon.payloads), 1)

    def test_agent_sudo_n_translates(self):
        self.start_daemon()
        self.run_cmd(["sudo", "-n", "make", "install"], via_agent=True)
        self.assertIn("pkexec make install", self.exec_log())
        self.assertNotIn("sudo -n", self.exec_log())
        self.assertEqual(len(self.daemon.payloads), 1)

    def test_agent_sudo_probe_only_passes_through(self):
        self.start_daemon()
        self.run_cmd(["sudo", "-nv"], via_agent=True)
        self.assertIn("sudo -nv", self.exec_log())
        self.assertNotIn("pkexec", self.exec_log())
        self.assertEqual(self.daemon.payloads, [])

    def test_agent_sudo_unsupported_option_passes_through(self):
        self.start_daemon()
        self.run_cmd(["sudo", "-E", "id"], via_agent=True)
        self.assertIn("sudo -E id", self.exec_log())
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

    # --- CLI mode (aisudo / bb-auth-declare argv0) ---

    def test_cli_bare_command_normalizes_to_sudo(self):
        r = self.run_cli(["make", "install"])
        self.assertIn("pkexec make install", self.exec_log())
        p = self.daemon.payloads[0]
        self.assertEqual(p["command"], "sudo make install")
        self.assertEqual(p["reason"], "sudo make install")
        self.assertIn("→ pkexec", r.stderr)

    def test_cli_sudo_n_translates(self):
        r = self.run_cli(["sudo", "-n", "make", "install"])
        self.assertIn("pkexec make install", self.exec_log())
        self.assertIn("dropped -n", r.stderr)

    def test_cli_bare_n_flag(self):
        r = self.run_cli(["-n", "make", "install"])
        self.assertIn("pkexec make install", self.exec_log())

    def test_cli_sudo_u_maps(self):
        self.run_cli(["sudo", "-u", "nobody", "id"])
        self.assertIn("pkexec --user nobody id", self.exec_log())

    def test_cli_decline_notes_reason(self):
        r = self.run_cli(["sudo", "-E", "id"])
        self.assertIn("sudo -E id", self.exec_log())
        self.assertIn("not translatable", r.stderr)

    def test_cli_explicit_reason(self):
        self.run_cli(["-r", "why", "sudo", "id"])
        self.assertEqual(self.daemon.payloads[0]["reason"], "why")

    def test_cli_daemon_down_execs_unchanged(self):
        r = self.run_cli(["sudo", "-n", "id"], daemon=False)
        self.assertIn("sudo -n id", self.exec_log())
        self.assertNotIn("pkexec", self.exec_log())
        self.assertIn("declaration unavailable", r.stderr)

    def test_cli_unbound_daemon_still_translates(self):
        self.start_daemon(bound=False)
        self.run_cli(["sudo", "id"], daemon=False)
        self.assertIn("pkexec id", self.exec_log())

    def test_cli_json_inline(self):
        self.run_cli(["--json", '{"argv": ["sudo", "id"], "reason": "why"}'])
        self.assertIn("pkexec id", self.exec_log())
        self.assertEqual(self.daemon.payloads[0]["reason"], "why")

    def test_cli_json_command_string(self):
        self.run_cli(["--json", '{"command": "sudo make install"}'])
        self.assertIn("pkexec make install", self.exec_log())

    def test_cli_json_stdin(self):
        self.run_cli(["--json", "-"], stdin_text='{"argv": ["sudo", "id"]}')
        self.assertIn("pkexec id", self.exec_log())

    def test_cli_json_unknown_key_corrective(self):
        r = self.run_cli(["--json", '{"args": ["sudo", "id"]}'])
        self.assertEqual(r.returncode, 2)
        self.assertIn("argv", r.stderr)
        self.assertEqual(self.exec_log(), "")

    def test_cli_json_malformed_corrective(self):
        r = self.run_cli(["--json", "{nope"])
        self.assertEqual(r.returncode, 2)
        self.assertIn("invalid --json", r.stderr)

    def test_cli_dry_run_no_side_effects(self):
        r = self.run_cli(["--dry-run", "--", "sudo", "-n", "make", "install"],
                         daemon=False)
        self.assertEqual(r.returncode, 0)
        plan = json.loads(r.stdout)
        self.assertEqual(plan["exec"], ["pkexec", "make", "install"])
        self.assertEqual(plan["fallback_exec"], ["sudo", "-n", "make", "install"])
        self.assertEqual(plan["declare"]["command"], "sudo -n make install")
        self.assertIn("dropped -n", plan["note"])
        self.assertEqual(self.exec_log(), "")

    def test_cli_declare_only_requires_reason(self):
        r = self.run_cli([], daemon=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("--reason is required", r.stderr)

    def test_cli_declare_only(self):
        r = self.run_cli(["-r", "why"])
        self.assertEqual(r.returncode, 0)
        self.assertEqual(len(self.daemon.payloads), 1)
        self.assertEqual(self.exec_log(), "")

    def test_aisudo_argv0_is_cli_mode(self):
        link = os.path.join(self.fakedir, "aisudo")
        os.symlink(SCRIPT, link)
        r = subprocess.run([link, "--dry-run", "id"], env=self.env,
                           timeout=15, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(json.loads(r.stdout)["exec"], ["pkexec", "id"])

    # --- unit-level ---

    def test_translate_rules(self):
        m = _load_module()
        self.assertEqual(m._translate("sudo", ["pacman", "-Syu"]),
                         (["pkexec", "pacman", "-Syu"], ""))
        self.assertEqual(m._translate("doas", ["id"]), (["pkexec", "id"], ""))
        # Supported sudo subset: -n drops, -u/--user map, -- consumed.
        self.assertEqual(m._translate("sudo", ["-n", "make", "install"]),
                         (["pkexec", "make", "install"], "dropped -n: GUI prompt replaces stdin auth"))
        self.assertEqual(m._translate("sudo", ["-u", "nobody", "id"]),
                         (["pkexec", "--user", "nobody", "id"], ""))
        self.assertEqual(m._translate("sudo", ["-unobody", "id"]),
                         (["pkexec", "--user", "nobody", "id"], ""))
        self.assertEqual(m._translate("sudo", ["--user=nobody", "id"]),
                         (["pkexec", "--user", "nobody", "id"], ""))
        self.assertEqual(m._translate("sudo", ["--", "id"]), (["pkexec", "id"], ""))
        self.assertEqual(m._translate("sudo", ["-n", "-u", "nobody", "id"]),
                         (["pkexec", "--user", "nobody", "id"],
                          "dropped -n: GUI prompt replaces stdin auth"))
        # Probe-only and unsupported forms decline.
        self.assertIsNone(m._translate("sudo", [])[0])
        self.assertIsNone(m._translate("sudo", ["-n"])[0])
        self.assertIsNone(m._translate("sudo", ["-nv"])[0])
        self.assertIsNone(m._translate("sudo", ["-E", "id"])[0])
        self.assertIsNone(m._translate("sudo", ["-i"])[0])
        self.assertIsNone(m._translate("sudo", ["FOO=1", "id"])[0])
        self.assertIsNone(m._translate("pkexec", ["id"])[0])
        self.assertIsNone(m._translate("sudo", ["-u"])[0])
        self.assertIsNone(m._translate("sudo", ["-u", "bad;user", "id"])[0])
        # Shell metacharacters in ANY argument must decline — parity with the
        # harness hook's refusal class.
        self.assertIsNone(m._translate("sudo", ["sh", "-c", "a|b"])[0])
        self.assertIsNone(m._translate("sudo", ["a|b"])[0])
        self.assertIsNone(m._translate("sudo", ["x", "&&", "y"])[0])
        self.assertIsNone(m._translate("sudo", ["$(whoami)"])[0])
        self.assertIsNone(m._translate("sudo", ["cmd", "`id`"])[0])
        self.assertIsNone(m._translate("sudo", ["cmd", ">out"])[0])
        # Options after the command are the command's own args.
        self.assertEqual(m._translate("sudo", ["cmd", "-n", "x"])[0],
                         ["pkexec", "cmd", "-n", "x"])

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
