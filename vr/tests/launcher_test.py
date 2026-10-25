#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test config tuning in a temporary package; never touches SteamVR/user files."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
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
    m.tune(SimpleNamespace(rate_control='bandwidth',bitrate_kbps=35000,head_height=None))
    v=json.loads(m.CONFIG.read_text())
    assert v['backend']['rate_control']=='bandwidth' and v['backend']['bitrate_kbps']==35000
    assert v['backend']['qp']==30 and v['backend']['key_file']=='unchanged-secret-path'
    before=m.CONFIG.read_bytes();settings_before=settings.read_bytes()
    for bad in [0,-1,1000001,1.5,'20000',True]:
        try:m.tune(SimpleNamespace(bitrate_kbps=bad,head_height=None))
        except ValueError:pass
        else:raise AssertionError(f'accepted invalid bitrate {bad!r}')
        assert m.CONFIG.read_bytes()==before and settings.read_bytes()==settings_before
    try:m.tune(SimpleNamespace(rate_control='adaptive',head_height=None))
    except ValueError:pass
    else:raise AssertionError('accepted unknown rate-control mode')
    assert m.CONFIG.read_bytes()==before
    m.tune(SimpleNamespace(rate_control='qp',head_height=None))
    assert json.loads(m.CONFIG.read_text())['backend']['qp']==30
    v=json.loads(m.CONFIG.read_text())
    v['backend'].update(bind='0.0.0.0',control_port=5021,video_port=5020,auth='key')
    for mode in ['bandwidth','qp',None]:
        if mode is None:
            v['backend'].pop('rate_control',None)
            v['backend'].pop('bitrate_kbps',None)
        else:
            v['backend'].update(rate_control=mode,bitrate_kbps=35000)
        m.write_json(m.CONFIG,v)
        with patch.dict(m.os.environ,{'XDG_RUNTIME_DIR':temporary,
                'NETDISPLAY_VR_RATE_CONTROL':'invalid','NETDISPLAY_VR_BITRATE_KBPS':'0'}):
            with patch.object(m.os,'execv') as execute:
                m.run()
                execute.assert_called_once()
                assert execute.call_args.args[1][0]==str(m.PACKAGE/'bin/netdisplay-vr-server')
                assert m.os.environ['NETDISPLAY_VR_RATE_CONTROL']==(mode or 'qp')
                assert m.os.environ['NETDISPLAY_VR_BITRATE_KBPS']==('20000' if mode is None else '35000')
                assert m.os.environ['NETDISPLAY_VR_QP']=='30'
print('PASS: launcher tune, rate-control switching, preserved pairing paths, validation and file permissions')

# Exercise installation, legacy migration, and restoration without SteamVR.
with tempfile.TemporaryDirectory() as temporary:
    root=Path(temporary); runtime=root/'runtime'; binary=runtime/'bin/linux64/vrcompositor'
    binary.parent.mkdir(parents=True); binary.write_bytes(b'original compositor')
    settings=root/'steamvr.vrsettings'; settings.write_text('{}')
    m.PACKAGE=root/'package'; m.PACKAGE.mkdir()
    m.CONFIG=m.PACKAGE/'session.json'; m.CONFIG.write_text('{}')
    m.STATE=m.PACKAGE/'installation.json'
    wrapper=m.PACKAGE/'compositor-wrapper'; wrapper.write_text('wrapper')
    m.paths=lambda:(runtime,settings)
    original=binary.parent/'netdisplay-original/vrcompositor'
    legacy=binary.with_name('vrcompositor.netdisplay-original')
    with patch.object(m.subprocess,'run'):
        m.install()
        assert binary.is_symlink() and binary.resolve()==wrapper
        assert original.read_bytes()==b'original compositor'
        original.rename(legacy)
        m.install()
        assert original.is_file() and not legacy.exists()
        m.restore()
        assert not binary.is_symlink() and binary.read_bytes()==b'original compositor'
        assert json.loads(settings.read_text())['steamvr']=={}
        m.install()
        original.rename(legacy)
        m.restore()
        assert binary.read_bytes()==b'original compositor' and not m.STATE.exists()
print('PASS: compositor basename, legacy migration, and reversible installation')
