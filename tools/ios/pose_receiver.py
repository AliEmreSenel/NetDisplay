#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect iPhone head pose; this does NOT install a SteamVR/OpenXR driver."""
import dataclasses
import json
import select
import sys
import time
from motion_protocol import arguments, make_receiver

def main():
    parser = arguments(__doc__)
    parser.add_argument("--json", action="store_true", help="emit the newest received pose as JSON")
    args = parser.parse_args()
    try:
        receiver = make_receiver(args)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"Listening for authenticated motion on {args.bind}:{args.port}. Ctrl-C to stop.", file=sys.stderr)
    start = time.monotonic(); previous = 0; last_print = 0; last_seq = None
    try:
        while True:
            select.select([receiver.socket], [], [], 0.25)
            pose = receiver.drain()
            now = time.monotonic()
            if args.json and pose and pose.sequence != last_seq:
                print(json.dumps(dataclasses.asdict(pose)), flush=True); last_seq = pose.sequence
            if not args.json and now - last_print >= 0.5:
                hz = (receiver.accepted - previous) / max(now-start, 0.001)
                if pose:
                    q = " ".join(f"{x:+.4f}" for x in pose.quaternion)
                    age = (now - pose.received_at) * 1000
                    print(f"{hz:6.1f} Hz  q_xyzw=[{q}]  local-age={age:6.1f} ms  "
                          f"recenter={pose.recenter_generation} bad={receiver.invalid} stale={receiver.stale}", flush=True)
                else:
                    print(f"Waiting. Invalid packets={receiver.invalid}; check token, peer and UDP firewall.", flush=True)
                start = now; previous = receiver.accepted; last_print = now
    except KeyboardInterrupt:
        pass
    finally:
        receiver.close()

if __name__ == "__main__":
    main()
