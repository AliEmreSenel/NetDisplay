#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Optional Linux/Tk GUI smoke test. Run under Xvfb; does not test iOS."""
from pathlib import Path
import hashlib
import hmac
import os
import socket
import sys
import tempfile
import tkinter as tk

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'ios'))
import motion_protocol
import pose_demo

key = bytes(range(32))
original_tk = tk.Tk
original_receiver = pose_demo.make_receiver
receivers = []
errors = []

def receiver(args):
    rx = original_receiver(args)
    receivers.append(rx)
    return rx

def test_tk(*args, **kwargs):
    root = original_tk(*args, **kwargs)
    root.report_callback_exception = lambda kind, value, tb: (errors.append(str(value)), root.destroy())
    def send():
        rx = receivers[0]
        packet = motion_protocol.BODY.pack(b'NDM1', 1, 1, 7, 123456, 100, 200,
            0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1)
        packet += hmac.digest(key, packet, hashlib.sha256)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(packet, ('127.0.0.1', rx.socket.getsockname()[1]))
    def finish():
        if not receivers or receivers[0].accepted != 1:
            errors.append('The demo did not accept the authenticated sample')
        canvases = [w for w in root.winfo_children() if isinstance(w, tk.Canvas)]
        if not canvases or len(canvases[0].find_all()) < 10:
            errors.append('The demo did not draw stereo geometry')
        root.destroy()
    root.after(60, send)
    root.after(500, finish)
    return root

with tempfile.TemporaryDirectory() as temp:
    token = Path(temp) / 'motion.key'
    token.write_text(key.hex()); token.chmod(0o600)
    # Port 0 is not a CLI option; acquire an unused local port for this short test.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    sys.argv = ['pose_demo.py', '--bind', '127.0.0.1', '--port', str(port),
                '--token-file', str(token), '--width', '800', '--height', '480']
    pose_demo.make_receiver = receiver
    tk.Tk = test_tk
    try:
        pose_demo.main()
    finally:
        tk.Tk = original_tk
        pose_demo.make_receiver = original_receiver
if errors:
    raise SystemExit('FAIL: ' + '; '.join(errors))
print('PASS: Linux Tk stereo demo launched, received HMAC pose, drew geometry and closed cleanly')
