#!/usr/bin/env python3
"""Exercise the actual backend's authentication and fixed stereo-mode negotiation."""
import hashlib
import hmac
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

MAGIC=0x4e444333
KEY=bytes(range(32))
def send(s,kind,payload=b''):
    s.sendall(struct.pack('!IHHI',MAGIC,6,kind,len(payload))+payload)
def exact(s,n):
    data=b''
    while len(data)<n:
        part=s.recv(n-len(data))
        if not part:raise EOFError('backend closed control')
        data+=part
    return data
def receive(s):
    magic,version,kind,size=struct.unpack('!IHHI',exact(s,12));assert (magic,version)==(MAGIC,6)
    return kind,exact(s,size)
def hello(s,flags=6):
    nonce=os.urandom(16);send(s,1,struct.pack('!IHHI',flags,1,0,1)+nonce)
    return nonce
# Match the domain-separated authentication contract in src/crypto.c.
def proof(role,client,server):
    return hashlib.blake2b(b'NetDisplay-v5-auth'+role+client+server,key=KEY,digest_size=32).digest()
def connect(port):
    s=socket.create_connection(('127.0.0.1',port),2);s.settimeout(3);return s

with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);(tmp/'key').write_text(KEY.hex())
    with socket.socket() as port_socket:
        port_socket.bind(('127.0.0.1',0));port=port_socket.getsockname()[1]
    with (tmp/'log').open('w+') as log:
        p=subprocess.Popen([sys.argv[1],'127.0.0.1','127.0.0.1',str(port),'55020','320','240','60',str(tmp/'key'),'key','libx264',str(tmp/'ipc')],stdout=log,stderr=log)
        try:
            for _ in range(100):
                if (tmp/'ipc').exists():break
                if p.poll() is not None:raise RuntimeError('backend exited')
                time.sleep(.02)
            with connect(port) as s:
                hello(s,4);assert receive(s)[0]==14, 'plaintext must be rejected'
            with connect(port) as s:
                hello(s);assert receive(s)[0]==9
                send(s,10,b'\0'*32);assert receive(s)[0]==14, 'wrong key must be rejected'
            for width,success in [(322,False),(320,True)]:
                with connect(port) as s:
                    cn=hello(s);kind,ch=receive(s);assert kind==9
                    sn=ch[4:];send(s,10,proof(b'client',cn,sn));kind,answer=receive(s)
                    assert kind==10 and answer==proof(b'server',cn,sn),'mutual authentication'
                    send(s,11,struct.pack('!IHHHH64s',1,width,240,60,0,b'iphone'))
                    kind,data=receive(s)
                    if not success:assert kind==14;continue
                    assert kind==2 and struct.unpack('!IHH',data)==(6,1,1)
                    kind,data=receive(s);assert kind==12
                    display,session,video,w,h,fps,name=struct.unpack('!IQHHHH64s',data)
                    assert (display,w,h,fps)==(1,320,240,60) and session
                    assert name.rstrip(b'\0')==b'netdisplay-vr-stereo'
                    send(s,13,struct.pack('!I',1));send(s,4);assert receive(s)==(5,b'')
                    send(s,6)
            print('VR backend: plaintext rejection, wrong-key rejection, mutual authentication, mode rejection, stereo negotiation, heartbeat and reconnect passed')
        except BaseException:
            log.flush();print((tmp/'log').read_text(),file=sys.stderr);raise
        finally:
            p.terminate();p.wait(timeout=8)
