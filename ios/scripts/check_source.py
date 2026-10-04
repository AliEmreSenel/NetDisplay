#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate packaging inputs without pretending to compile Apple's frameworks."""
from pathlib import Path
import json
import plistlib
import re
import xml.etree.ElementTree as ET

root=Path(__file__).resolve().parents[1]
project=root/'NetDisplay.xcodeproj'/'project.pbxproj'
text=project.read_text()
for folder in ('App','Core'):
    for path in (root/folder).glob('*.swift'):
        assert str(path.relative_to(root)) in text, f'Missing Xcode source: {path}'
assert '../src/crypto.c' in text and 'App/Viewer.metal' in text
assert '"IPHONEOS_DEPLOYMENT_TARGET" = "17.0"' in text, 'Hardware-decoder requirement keys need iOS 17'
assert 'CODE_SIGNING_ALLOWED' in text
ET.parse(root/'NetDisplay.xcodeproj'/'xcshareddata'/'xcschemes'/'NetDisplay.xcscheme')
info=plistlib.loads((root/'App'/'Info.plist').read_bytes())
assert info['NSLocalNetworkUsageDescription'] and info['NSMotionUsageDescription']
assert info['NSCameraUsageDescription'], 'Optional AR tracking needs camera permission'
assert len(info['UISupportedInterfaceOrientations'])==2
privacy=plistlib.loads((root/'App'/'PrivacyInfo.xcprivacy').read_bytes())
assert privacy['NSPrivacyTracking'] is False
assert len(privacy['NSPrivacyAccessedAPITypes'])==2
for path in (root/'App'/'Assets.xcassets').rglob('Contents.json'):
    content=json.loads(path.read_text())
    for image in content.get('images',[]):
        if 'filename' in image: assert (path.parent/image['filename']).is_file()
assert re.search(r'#define\s+NDC_VERSION\s+6u', (root.parent/'src'/'control_proto.h').read_text())
assert 'sendmmsg' not in (root/'Native'/'NDNative.c').read_text()
assert 'workflow_dispatch:' in (root.parent/'.github'/'workflows'/'ios.yml').read_text()
assert 'secrets.' not in (root.parent/'.github'/'workflows'/'ios.yml').read_text()
print('PASS: Xcode source membership, scheme, permissions, privacy manifest, assets, protocol version and credential-free workflow')
