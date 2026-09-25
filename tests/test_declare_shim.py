#!/usr/bin/env python3
"""Tests for the aisudo CLI and shim mode (PATH interception of sudo/doas/pkexec).

Runs the real bb-auth-agent binary through symlinks named sudo/doas/pkexec under a staged
PATH, with stub "real" binaries logging their argv and a stub daemon socket
recording declarations. The agent-ancestor path is exercised via a parent
process whose cmdline contains a known agent signature.
"""
from __future__ import annotations

import json
import os
import shlex
import socket
import subprocess
import tempfile
import threading
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
# The bb-auth-agent binary under its `aisudo` name (ctest passes the build-tree
# symlink). Symlinks named sudo/doas/pkexec select shim mode via argv[0].
SCRIPT = os.path.abspath(os.environ.get("AISUDO", "aisudo"))
FIXTURE = os.path.join(_HERE, "fixtures", "escalation-translation.json")


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
        """Run the shim with agent detection pinned off — deterministic human
        passthrough even when the ambient test process sits under an agent
        (e.g. agent-driven CI)."""
        return subprocess.run(argv, env={**self.env, "BB_AUTH_TEST_ASSUME_HUMAN": "1"},
                              timeout=15, capture_output=True, text=True)

    def dry_run(self, argv):
        r = self.run_cli(["--dry-run", "--"] + argv, daemon=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

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

    # --- CLI mode (aisudo argv0) ---

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

    # --- translation (via --dry-run: no daemon, no exec) ---

    def test_translate_rules(self):
        self.assertEqual(self.dry_run(["sudo", "pacman", "-Syu"])["exec"], ["pkexec", "pacman", "-Syu"])
        plan = self.dry_run(["sudo", "-n", "-u", "nobody", "id"])
        self.assertEqual(plan["exec"], ["pkexec", "--user", "nobody", "id"])
        self.assertEqual(plan["note"], "dropped -n: GUI prompt replaces stdin auth")
        # Declines keep the original launcher and say why.
        plan = self.dry_run(["sudo", "-E", "id"])
        self.assertEqual(plan["exec"], ["sudo", "-E", "id"])
        self.assertIn("not translatable", plan["note"])
        self.assertNotIn("fallback_exec", plan)
        self.assertIn("probe", self.dry_run(["sudo", "-n"])["note"])
        self.assertEqual(self.dry_run(["sudo", "cmd", "-n", "x"])["exec"], ["pkexec", "cmd", "-n", "x"])

    # @lat: [[tests#Agent handoff#Shared translation vectors]]
    def test_shared_translation_vectors(self):
        with open(FIXTURE) as f:
            rows = json.load(f)["rows"]
        for row in rows:
            with self.subTest(command=row["command"]):
                argv = shlex.split(row["command"])
                want = shlex.split(row["rewrite"]) if row["rewrite"] else argv
                self.assertEqual(self.dry_run(argv)["exec"], want)

    def test_print_shim_dir(self):
        out = subprocess.run([SCRIPT, "--print-shim-dir"], capture_output=True, text=True, timeout=15)
        self.assertEqual(out.returncode, 0)
        self.assertTrue(out.stdout.strip().endswith("bb-auth-shims"))


if __name__ == "__main__":
    unittest.main()
