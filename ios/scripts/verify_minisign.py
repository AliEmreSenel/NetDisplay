#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify Minisign release signatures with the host libsodium library.

Used when the minisign command is absent. Both the file signature and signed
trusted comment are checked; no cryptographic primitives are implemented here.
"""
import argparse
import base64
import binascii
import ctypes
import ctypes.util
import hashlib
from pathlib import Path
import subprocess


def host_sodium():
    name = ctypes.util.find_library('sodium')
    if not name:
        try:
            directory = subprocess.check_output(
                ['pkg-config', '--variable=libdir', 'libsodium'], text=True
            ).strip()
            name = next((str(p) for p in Path(directory).glob('libsodium.*')
                         if p.suffix in ('.so', '.dylib')), None)
        except (OSError, subprocess.CalledProcessError):
            pass
    if not name:
        raise ValueError('Install minisign or the host libsodium library to verify release signatures.')
    library = ctypes.CDLL(name)
    library.crypto_sign_verify_detached.argtypes = (
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p
    )
    library.crypto_sign_verify_detached.restype = ctypes.c_int
    return library


def verify(data, signature_text, public_key):
    lines = signature_text.splitlines()
    if (len(lines) != 4 or not lines[0].startswith('untrusted comment: ')
            or not lines[2].startswith('trusted comment: ')):
        raise ValueError('Invalid Minisign signature format')
    try:
        key = base64.b64decode(public_key, validate=True)
        signature = base64.b64decode(lines[1], validate=True)
        comment_signature = base64.b64decode(lines[3], validate=True)
    except (ValueError, binascii.Error) as error:
        raise ValueError('Invalid Minisign base64') from error
    if (len(key) != 42 or key[:2] != b'Ed' or len(signature) != 74
            or signature[:2] not in (b'Ed', b'ED') or len(comment_signature) != 64
            or signature[2:10] != key[2:10]):
        raise ValueError('Minisign algorithm, key ID or signature length mismatch')
    sodium = host_sodium()
    message = hashlib.blake2b(data).digest() if signature[:2] == b'ED' else data
    if sodium.crypto_sign_verify_detached(signature[10:], message, len(message), key[10:]) != 0:
        raise ValueError('Release signature verification failed')
    trusted = signature[10:] + lines[2][len('trusted comment: '):].encode('utf-8')
    if sodium.crypto_sign_verify_detached(comment_signature, trusted, len(trusted), key[10:]) != 0:
        raise ValueError('Minisign trusted-comment verification failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('signature', type=Path)
    parser.add_argument('public_key')
    args = parser.parse_args()
    try:
        verify(args.archive.read_bytes(), args.signature.read_text(), args.public_key)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print('Verified upstream Minisign file and trusted-comment signatures')


if __name__ == '__main__':
    main()
