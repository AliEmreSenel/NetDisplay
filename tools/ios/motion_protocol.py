# SPDX-License-Identifier: GPL-2.0-only
"""NetDisplay iOS motion v1. No third-party Python dependencies."""
from __future__ import annotations
import argparse
import collections
import hashlib
import hmac
import math
from pathlib import Path
import socket
import struct
import sys
import time
from dataclasses import dataclass

BODY = struct.Struct("!4sHHIQQQ14fI")
PACKET_SIZE = BODY.size + 32
assert BODY.size == 96 and PACKET_SIZE == 128

@dataclass(frozen=True)
class Pose:
    sequence: int
    session: int
    sample_ns: int
    send_ns: int
    quaternion: tuple[float, float, float, float]
    angular_velocity: tuple[float, float, float]
    acceleration: tuple[float, float, float]
    raw_quaternion: tuple[float, float, float, float]
    recenter_generation: int
    camera_on_left: bool
    received_at: float


def decode_packet(packet: bytes, key: bytes, now: float | None = None) -> Pose:
    if len(key) != 32 or len(packet) != PACKET_SIZE:
        raise ValueError("Wrong key or packet length")
    body, tag = packet[:BODY.size], packet[BODY.size:]
    if not hmac.compare_digest(hmac.digest(key, body, hashlib.sha256), tag):
        raise ValueError("Motion authentication failed")
    magic, version, flags, seq, session, sample, sent, *rest = BODY.unpack(body)
    values, generation = rest[:14], rest[14]
    if magic != b"NDM1" or version != 1 or flags & ~1 or not session:
        raise ValueError("Unsupported motion header")
    if not all(math.isfinite(v) for v in values):
        raise ValueError("Non-finite motion sample")
    q = tuple(values[:4])
    raw = tuple(values[10:14])
    if not 0.8 < sum(x*x for x in q) < 1.2 or not 0.8 < sum(x*x for x in raw) < 1.2:
        raise ValueError("Invalid quaternion")
    return Pose(seq, session, sample, sent, q, tuple(values[4:7]), tuple(values[7:10]),
                raw, generation, bool(flags & 1), time.monotonic() if now is None else now)


class Receiver:
    """Nonblocking, HMAC-verified newest-pose receiver with per-session replay rejection."""
    def __init__(self, host: str, port: int, key: bytes, peer: str | None = None):
        self.key, self.peer = key, peer
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((host, port))
        self.socket.setblocking(False)
        self.latest: Pose | None = None
        self.accepted = self.invalid = self.stale = 0
        self.active_ip: str | None = None
        self.retired: collections.deque[int] = collections.deque(maxlen=64)

    def drain(self) -> Pose | None:
        # Bound the drain so a busy network cannot starve rendering.
        for _ in range(512):
            try:
                packet, address = self.socket.recvfrom(PACKET_SIZE + 1)
            except BlockingIOError:
                break
            if self.peer is not None and address[0] != self.peer:
                self.invalid += 1
                continue
            try:
                pose = decode_packet(packet, self.key)
            except ValueError:
                self.invalid += 1
                continue
            if self.latest is not None:
                if pose.session != self.latest.session:
                    # A fresh app session is accepted after a brief quiet period.
                    if pose.session in self.retired or pose.received_at - self.latest.received_at < 1.0:
                        self.stale += 1
                        continue
                    self.retired.append(self.latest.session)
                else:
                    step = (pose.sequence - self.latest.sequence) & 0xffffffff
                    if step == 0 or step >= 0x80000000 or address[0] != self.active_ip:
                        self.stale += 1
                        continue
            self.latest, self.active_ip = pose, address[0]
            self.accepted += 1
        return self.latest

    def close(self):
        self.socket.close()


def arguments(description: str) -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--bind", default="0.0.0.0", help="local address; bind only to the USB IP for tighter isolation")
    p.add_argument("--port", type=int, default=5010)
    p.add_argument("--peer", help="accept packets only from this iPhone IPv4 address")
    p.add_argument("--token-file", required=True, type=Path, help="private file containing the 64-hex-character token copied from iOS")
    return p


def make_receiver(args) -> Receiver:
    if not 1 <= args.port <= 65535:
        raise ValueError("Port must be 1..65535")
    text = args.token_file.read_text().strip()
    if len(text) != 64:
        raise ValueError("Token file must contain 64 hexadecimal characters")
    key = bytes.fromhex(text)
    if args.token_file.stat().st_mode & 0o077:
        print("Warning: motion token is readable by other users; run chmod 600 on the token file.", file=sys.stderr)
    return Receiver(args.bind, args.port, key, args.peer)


def multiply(a, b):
    x,y,z,w = a; X,Y,Z,W = b
    return (w*X+x*W+y*Z-z*Y, w*Y-x*Z+y*W+z*X,
            w*Z+x*Y-y*X+z*W, w*W-x*X-y*Y-z*Z)


def rotate(q, point):
    x,y,z,w = q
    return multiply(multiply(q, (*point, 0)), (-x,-y,-z,w))[:3]
