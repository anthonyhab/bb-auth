#!/usr/bin/env python3
"""Minimal bb-auth external provider template.

This process connects to the daemon socket and stays registered with heartbeat.
It is intentionally minimal and does not render UI or submit responses.
"""

import argparse
import json
import socket
import sys
import time
from typing import Any, Dict


def send_line(sock: socket.socket, payload: Dict[str, Any]) -> None:
    data = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")
    sock.sendall(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="bb-auth provider template")
    parser.add_argument("--socket", required=True, help="Path to bb-auth socket")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(args.socket)
    sock.settimeout(0.5)

    send_line(sock, {"type": "ui.register", "name": "provider-template", "kind": "custom", "priority": 20})
    send_line(sock, {"type": "subscribe"})

    last_heartbeat = 0.0
    try:
        while True:
            now = time.monotonic()
            if now - last_heartbeat >= 2.0:
                send_line(sock, {"type": "ui.heartbeat"})
                last_heartbeat = now

            try:
                data = sock.recv(4096)
                if not data:
                    return 0
                # Messages are ignored in this template provider.
            except TimeoutError:
                pass
    except (BrokenPipeError, ConnectionError, OSError):
        return 0
    finally:
        try:
            sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
