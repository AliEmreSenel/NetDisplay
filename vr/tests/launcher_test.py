#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test config tuning in a temporary package; never touches SteamVR/user files."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
p=Path(__file__).resolve().parents[1]/'netdisplay-vr.py'
spec=importlib.util.spec_from_file_location('launcher',p)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory() as temporary:
    m.PACKAGE=Path(temporary);m.CONFIG=m.PACKAGE/'session.json'
    m.require_stopped=lambda:None
    def absent(): raise OSError('no installed SteamVR in test')
    m.paths=absent
    m.write_json(m.CONFIG, {'backend':{'width':1920,'height':1080,'fps':60,'encoder':'hevc_nvenc',
        'key_file':'unchanged-secret-path','peer':'127.0.0.1'},'steamvr_hmd_init_config':{}})
    settings=m.PACKAGE/'driver/resources/settings/default.vrsettings'
    m.write_json(settings,{'driver_netdisplay_vr':{'tokenFile':'unchanged-token-path'}})
    m.tune(SimpleNamespace(width=1280,height=720,fps=60,qp=30,encoder='h264_nvenc',head_height=1.65))
    v=json.loads(m.CONFIG.read_text());h=json.loads(settings.read_text())['driver_netdisplay_vr']
    assert v['backend']['key_file']=='unchanged-secret-path' and h['tokenFile']=='unchanged-token-path'
    assert v['backend']['width']==1280 and v['steamvr_hmd_init_config']['eye_resolution_width']==640
    assert h['frequency']==60 and h['headHeight']==1.65
    before=m.CONFIG.read_bytes()
    try:m.tune(SimpleNamespace(width=1281,height=None,fps=None,qp=None,encoder=None,head_height=None))
    except ValueError:pass
    else:raise AssertionError('accepted odd width')
    assert m.CONFIG.read_bytes()==before
    assert m.CONFIG.stat().st_mode & 0o777 == 0o600
print('PASS: launcher tune, matching capture/HMD mode, preserved pairing paths, validation and file permissions')
