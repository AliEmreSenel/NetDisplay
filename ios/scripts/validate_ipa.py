#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import plistlib
import sys
import zipfile
from pathlib import PurePosixPath

with zipfile.ZipFile(sys.argv[1]) as z:
    names = set(z.namelist())
    base = 'Payload/NetDisplay.app/'
    assert base+'Info.plist' in names, 'Missing app Info.plist'
    info = plistlib.loads(z.read(base+'Info.plist'))
    assert base+info['CFBundleExecutable'] in names, 'Missing executable'
    assert base+'default.metallib' in names, 'Metal shaders not compiled'
    assert info['MinimumOSVersion'].split('.')[0].isdigit()
    assert 'NSLocalNetworkUsageDescription' in info and 'NSMotionUsageDescription' in info
    assert not any(PurePosixPath(n).name == 'embedded.mobileprovision' for n in names), 'IPA unexpectedly contains a provisioning profile'
    assert all(not PurePosixPath(n).is_absolute() and '..' not in PurePosixPath(n).parts for n in names)
    print('PASS: IPA layout, executable, Metal library, permissions, and unsigned packaging')
