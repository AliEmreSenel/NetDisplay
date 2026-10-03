#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import hmac
import importlib.util
from pathlib import Path
import socket
import struct
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'/'ios'))
from motion_protocol import BODY,PACKET_SIZE,Receiver,decode_packet
build=Path(sys.argv[1])
key=bytes(range(32));cn=bytes(range(16));sn=bytes(range(16,32))
expected={role:hashlib.blake2b(b'NetDisplay-v5-auth'+role.encode()+cn+sn,key=key,digest_size=32).hexdigest() for role in ('client','server')}
expected['stream']=hashlib.blake2b(b'NetDisplay-v5-video'+cn+sn+(0x0102030405060708).to_bytes(8,'big'),key=key,digest_size=32).hexdigest()
actual=dict(line.split() for line in (build/'crypto-vectors.txt').read_text().splitlines())
assert actual==expected, 'Original C crypto does not match independent BLAKE2b vectors'
packet=(build/'motion-packet.bin').read_bytes()
assert len(packet)==128
pose=decode_packet(packet,key)
assert pose.sequence==7 and pose.session==0x0102030405060708 and pose.sample_ns==123 and pose.send_ns==456
assert pose.angular_velocity==(1,2,3) and pose.recenter_generation==2 and pose.quaternion==(0,0,0,1)
assert hmac.compare_digest(packet[-32:],hmac.digest(key,packet[:-32],'sha256'))
for bad in (packet[:-1],packet+b'X',packet[:-1]+bytes([packet[-1]^1])):
    try: decode_packet(bad,key)
    except ValueError: pass
    else: raise AssertionError('Accepted malformed/unauthenticated motion')
rx=Receiver('127.0.0.1',0,key,peer='127.0.0.1');s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
try:
    destination=rx.socket.getsockname()
    s.sendto(packet,destination);time.sleep(.01);assert rx.drain() is not None and rx.accepted==1
    s.sendto(packet,destination);time.sleep(.01);rx.drain();assert rx.accepted==1 and rx.stale==1
    broken=bytearray(packet);broken[50]^=1;s.sendto(broken,destination);time.sleep(.01);rx.drain();assert rx.invalid==1
    body=bytearray(packet[:96]);struct.pack_into('!I',body,8,8)
    new=bytes(body)+hmac.digest(key,body,'sha256');s.sendto(new,destination);time.sleep(.01)
    assert rx.drain().sequence==8 and rx.accepted==2
finally:
    rx.close();s.close()
print('PASS: C/Swift/Python wire interoperability, independent crypto vectors, UDP loopback and replay rejection')
