#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Swift body -> Python HMAC/parser -> C++ fixture; no Apple frameworks needed."""
import hashlib, hmac, json, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "ios"))
from motion_protocol import decode_packet
out = Path(sys.argv[1]); key = bytes(range(32))
def signed(body): return body + hmac.digest(key, body, hashlib.sha256)
body = (out / "motion-v2-body.bin").read_bytes()
p = signed(body); pose = decode_packet(p, key, 10)
assert len(p) == 160 and pose.spatial and pose.position_valid and pose.orientation_valid
assert pose.position == (.25, -.125, -.5) and pose.tracking_quality == 2
assert pose.recenter_generation == 2 and pose.sequence == 7
(out / "motion-v2-packet.bin").write_bytes(p)
# Produce the old fixture too for standalone portable C++ verification.
(out / "motion-packet.bin").write_bytes(signed((out / "motion-body.bin").read_bytes()))
checks = 4
for offset, value in [(120, 1), (124, 1), (96, 0x7fc00000), (108, 0x42000000)]:
    bad = bytearray(body); struct.pack_into("!I", bad, offset, value)
    try: decode_packet(signed(bad), key)
    except ValueError: checks += 1
    else: raise AssertionError(f"accepted invalid field {offset}")
for packet in [p[:-1], p+b"0", p[:100]+bytes([p[100]^1])+p[101:]]:
    try: decode_packet(packet, key)
    except ValueError: checks += 1
    else: raise AssertionError("accepted wrong size/auth")
limited = bytearray(body); limited[7]=1; struct.pack_into("!I", limited,120,1)
x = decode_packet(signed(limited), key)
assert x.spatial and not x.position_valid and not x.orientation_valid
checks += 1
print(f"PASS: {checks} spatial wire checks; v1/v2 C++ fixtures written")
