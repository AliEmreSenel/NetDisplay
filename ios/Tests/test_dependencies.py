#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tamper checks for authenticated build-dependency downloads."""
import base64
import ctypes
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from verify_minisign import host_sodium, verify


class ReleaseSignatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sodium = host_sodium()
        cls.sodium.crypto_sign_keypair.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
        cls.sodium.crypto_sign_keypair.restype = ctypes.c_int
        cls.sodium.crypto_sign_detached.argtypes = (
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_ulonglong, ctypes.c_void_p
        )
        cls.sodium.crypto_sign_detached.restype = ctypes.c_int
        cls.public = ctypes.create_string_buffer(32)
        cls.secret = ctypes.create_string_buffer(64)
        assert cls.sodium.crypto_sign_keypair(cls.public, cls.secret) == 0
        cls.key = base64.b64encode(b'Ed'+b'test-key'+cls.public.raw).decode()

    def sign(self, message):
        signature = ctypes.create_string_buffer(64)
        self.assertEqual(self.sodium.crypto_sign_detached(
            signature, None, message, len(message), self.secret), 0)
        return signature.raw

    def release(self, data, algorithm=b'ED'):
        message = hashlib.blake2b(data).digest() if algorithm == b'ED' else data
        signature = self.sign(message)
        comment = 'timestamp:1\tfile:release.tar.gz'
        return '\n'.join([
            'untrusted comment: release signature',
            base64.b64encode(algorithm+b'test-key'+signature).decode(),
            'trusted comment: '+comment,
            base64.b64encode(self.sign(signature+comment.encode())).decode(),
        ])+'\n'

    def test_prehash_and_legacy_signatures(self):
        for algorithm in (b'Ed', b'ED'):
            verify(b'release', self.release(b'release', algorithm), self.key)

    def test_modified_release_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Release signature'):
            verify(b'tampered release', self.release(b'release'), self.key)

    def test_modified_trusted_comment_is_rejected(self):
        signature = self.release(b'release').replace('timestamp:1', 'timestamp:2')
        with self.assertRaisesRegex(ValueError, 'trusted-comment'):
            verify(b'release', signature, self.key)

    def test_untrusted_comment_is_only_metadata(self):
        signature = self.release(b'release').replace('release signature', 'different metadata')
        verify(b'release', signature, self.key)

    def test_wrong_key_id_is_rejected(self):
        other = base64.b64encode(b'Ed'+b'otherkey'+self.public.raw).decode()
        with self.assertRaisesRegex(ValueError, 'key ID'):
            verify(b'release', self.release(b'release'), other)

    def test_malformed_signature_is_rejected(self):
        signature = self.release(b'release').splitlines()
        signature[1] = '!invalid!'
        with self.assertRaisesRegex(ValueError, 'base64'):
            verify(b'release', '\n'.join(signature), self.key)


if __name__ == '__main__':
    unittest.main()
