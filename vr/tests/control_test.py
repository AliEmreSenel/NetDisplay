#!/usr/bin/env python3
"""Exercise the actual backend's authentication and fixed stereo-mode negotiation.

The receiver side of the handshake is implemented independently here, from
the protocol description in src/crypto.c: libsodium (through ctypes) supplies
only the ristretto255 group and XChaCha20-Poly1305; every derivation uses
Python's own BLAKE2b and SHA-512.
"""
import ctypes
import ctypes.util
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

MAGIC=0x4e444333
VERSION=7
SEALED=23
KEY=bytes(range(32))

sodium=ctypes.CDLL(ctypes.util.find_library('sodium') or 'libsodium.so')
assert sodium.sodium_init()>=0
buf=ctypes.c_char_p
sodium.crypto_core_ristretto255_from_hash.argtypes=[buf,buf]
sodium.crypto_core_ristretto255_scalar_random.argtypes=[buf]
sodium.crypto_scalarmult_ristretto255.argtypes=[buf,buf,buf]
aead=[buf,ctypes.POINTER(ctypes.c_ulonglong),buf,ctypes.c_ulonglong,buf,ctypes.c_ulonglong,buf,buf,buf]
sodium.crypto_aead_xchacha20poly1305_ietf_encrypt.argtypes=aead
sodium.crypto_aead_xchacha20poly1305_ietf_decrypt.argtypes=[buf,ctypes.POINTER(ctypes.c_ulonglong),buf,buf,ctypes.c_ulonglong,buf,ctypes.c_ulonglong,buf,buf]

def scalarmult(scalar,point):
    out=ctypes.create_string_buffer(32)
    if sodium.crypto_scalarmult_ristretto255(out,scalar,point)!=0:raise ValueError('invalid share')
    return out.raw
def generator(key,client,server):
    out=ctypes.create_string_buffer(32)
    sodium.crypto_core_ristretto255_from_hash(out,hashlib.sha512(b'NetDisplay-v7-generator'+key+client+server).digest())
    return out.raw
def subkey(master,index):
    # libsodium's crypto_kdf_derive_from_key, context "NDv7keys".
    return hashlib.blake2b(key=master,digest_size=32,salt=index.to_bytes(8,'little')+bytes(8),person=b'NDv7keys'+bytes(8)).digest()
def nonce(seq):return b'NDc7'+seq.to_bytes(8,'big')+bytes(12)
def header(size):return struct.pack('!IHHI',MAGIC,VERSION,SEALED,size)

class Channel:
    def __init__(self,s):self.s,self.tx,self.rx,self.tx_seq,self.rx_seq=s,None,None,0,0
    def send(self,kind,payload=b''):
        if self.tx is None:
            self.s.sendall(struct.pack('!IHHI',MAGIC,VERSION,kind,len(payload))+payload);return
        plain=struct.pack('!H',kind)+payload;head=header(len(plain)+16)
        out=ctypes.create_string_buffer(len(plain)+16);n=ctypes.c_ulonglong()
        assert sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(out,ctypes.byref(n),plain,len(plain),head,12,None,nonce(self.tx_seq),self.tx)==0
        self.tx_seq+=1;self.s.sendall(head+out.raw[:n.value])
    def exact(self,n):
        data=b''
        while len(data)<n:
            part=self.s.recv(n-len(data))
            if not part:raise EOFError('backend closed control')
            data+=part
        return data
    def receive(self):
        head=self.exact(12);magic,version,kind,size=struct.unpack('!IHHI',head);assert (magic,version)==(MAGIC,VERSION)
        body=self.exact(size)
        if self.rx is None:return kind,body
        assert kind==SEALED,'plaintext after authentication'
        out=ctypes.create_string_buffer(max(size,1));n=ctypes.c_ulonglong()
        assert sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(out,ctypes.byref(n),None,body,size,head,12,nonce(self.rx_seq),self.rx)==0,'sealed message failed authentication'
        self.rx_seq+=1;plain=out.raw[:n.value]
        return struct.unpack('!H',plain[:2])[0],plain[2:]

def handshake(ch,flags=6,key=KEY):
    """Returns the source's answer type: 10 after mutual authentication."""
    client=os.urandom(16);hello=struct.pack('!IHHI',flags,1,0,1)+client
    ch.send(1,hello);kind,challenge=ch.receive()
    if kind!=9:return kind
    _,server,salt,share=struct.unpack('!I16s16s32s',challenge)
    assert salt==bytes(16),'a PSK source sends no password salt'
    scalar=ctypes.create_string_buffer(32);sodium.crypto_core_ristretto255_scalar_random(scalar)
    mine=scalarmult(scalar.raw,generator(key,client,server));shared=scalarmult(scalar.raw,share)
    transcript=hashlib.blake2b(b'NetDisplay-v7-transcript'+hello+challenge+mine,digest_size=32).digest()
    master=hashlib.blake2b(b'NetDisplay-v7-session'+transcript+key,key=shared,digest_size=32).digest()
    ch.send(10,mine+subkey(master,3));kind,answer=ch.receive()
    if kind!=10:return kind
    assert answer==subkey(master,4),'source proof'
    ch.tx,ch.rx=subkey(master,1),subkey(master,2)
    return kind
def connect(port):
    s=socket.create_connection(('127.0.0.1',port),2);s.settimeout(3);return Channel(s)

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
            ch=connect(port)
            with ch.s:assert handshake(ch,4)==14,'plaintext must be rejected'
            ch=connect(port)
            with ch.s:assert handshake(ch,key=bytes(32))==14,'wrong key must be rejected'
            ch=connect(port)
            with ch.s:
                assert handshake(ch)==10
                ch.tx=None;ch.send(11,struct.pack('!IHHHH64s',1,320,240,60,0,b'iphone'))
                try:ch.receive();raise AssertionError('plaintext after authentication must be refused')
                except (EOFError,ConnectionError):pass
            for width,success,predicted in [(322,False,False),(320,True,False),(320,True,True)]:
                ch=connect(port)
                with ch.s:
                    assert handshake(ch,6 | (128 if predicted else 0))==10,'mutual authentication'
                    ch.send(11,struct.pack('!IHHHH64s',1,width,240,60,0,b'iphone'))
                    kind,data=ch.receive()
                    if not success:assert kind==14;continue
                    enabled=predicted and int(os.environ.get('NETDISPLAY_VR_KEYFRAME_INTERVAL','3'))>1
                    assert kind==2 and struct.unpack('!IHH',data)==(6 | (128 if enabled else 0),1,1)
                    kind,data=ch.receive();assert kind==12
                    display,session,video,w,h,fps,name=struct.unpack('!IQHHHH64s',data)
                    assert (display,w,h,fps)==(1,320,240,60) and session
                    assert name.rstrip(b'\0')==b'netdisplay-vr-stereo'
                    ch.send(13,struct.pack('!I',1));ch.send(4);assert ch.receive()==(5,b'')
                    ch.send(6)
            print('VR backend: plaintext rejection, wrong-key rejection, PAKE mutual authentication, sealed control, mode rejection, stereo negotiation, heartbeat and reconnect passed')
        except BaseException:
            log.flush();print((tmp/'log').read_text(),file=sys.stderr);raise
        finally:
            p.terminate();p.wait(timeout=8)
