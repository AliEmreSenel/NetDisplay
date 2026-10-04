#!/usr/bin/env python3
"""Manage the dedicated NetDisplay VR backend and reversible SteamVR capture wrapper."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

PACKAGE = Path(__file__).resolve().parent
STATE = PACKAGE / 'installation.json'
CONFIG = PACKAGE / 'session.json'

def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.chmod(0o600)
    temporary.replace(path)

def paths():
    data = json.loads((Path.home()/'.config/openvr/openvrpaths.vrpath').read_text())
    return Path(data['runtime'][0]), Path(data['config'][0])/'steamvr.vrsettings'

def require_stopped():
    if subprocess.run(['pgrep', '-u', str(os.getuid()), '-x', 'vrserver'], stdout=subprocess.DEVNULL).returncode == 0:
        raise RuntimeError('Exit SteamVR before installing, configuring or restoring its driver.')

def token(source, target):
    value = Path(source).expanduser().read_text().strip()
    if len(value) != 64 or any(c not in '0123456789abcdefABCDEF' for c in value):
        raise ValueError(f'{source}: expected exactly 64 hex characters')
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f: f.write(value + '\n')
    Path(target).chmod(0o600)

def configure(a):
    import ipaddress
    require_stopped()
    ipaddress.IPv4Address(a.bind); ipaddress.IPv4Address(a.peer)
    if not (2 <= a.width <= 4096 and 2 <= a.height <= 2160 and a.width % 2 == 0 and a.height % 2 == 0 and 30 <= a.fps <= 120):
        raise ValueError('Use even dimensions up to 4096x2160, and 30–120 fps')
    if not all(1 <= p <= 65535 for p in [a.control_port,a.video_port,a.motion_port]): raise ValueError('Invalid port')
    if not 0.3 <= a.head_height <= 2.5 or not 0 <= a.qp <= 51: raise ValueError('head-height must be 0.3..2.5 m and QP 0..51')
    private=PACKAGE/'private';private.mkdir(exist_ok=True,mode=0o700);private.chmod(0o700)
    token(a.video_key_file,private/'video.key');token(a.motion_token_file,private/'motion.key')
    settings=dict(enable=True,width=a.width,height=a.height,windowX=0,windowY=0,frequency=float(a.fps),ipd=.064,verticalFov=90.,motionPort=a.motion_port,bindAddress=a.bind,peerAddress=a.peer,tokenFile=str(private/'motion.key'),headHeight=a.head_height)
    write_json(PACKAGE/'driver/resources/settings/default.vrsettings',{'driver_netdisplay_vr':settings})
    write_json(CONFIG,dict(steamvr_hmd_init_config=dict(eye_resolution_width=a.width//2,eye_resolution_height=a.height,refresh_rate=a.fps),backend=dict(bind=a.bind,peer=a.peer,control_port=a.control_port,video_port=a.video_port,width=a.width,height=a.height,fps=a.fps,key_file=str(private/'video.key'),auth=a.auth,encoder=a.encoder,qp=a.qp)))
    print(f'VR configured: {a.width}x{a.height}@{a.fps}, phone {a.peer}, control port {a.control_port}.')

def install():
    require_stopped()
    if not CONFIG.exists():raise RuntimeError('Run configure first')
    runtime, settings=paths(); compositor=runtime/'bin/linux64/vrcompositor';original=compositor.with_name('vrcompositor.netdisplay-original')
    wrapper=PACKAGE/'compositor-wrapper'
    if STATE.exists():
        if compositor.is_symlink() and compositor.resolve()==wrapper:print('VR integration already installed');return
        raise RuntimeError('SteamVR changed after installation; restore before reinstalling')
    if compositor.is_symlink() or original.exists():raise RuntimeError('Another compositor wrapper or backup exists; refusing to overwrite it')
    data=json.loads(settings.read_text()) if settings.exists() else {}
    changes=[('steamvr','forcedDriver','netdisplay_vr'),('steamvr','enableLinuxVulkanAsync',False),('steamvr','disableAsyncReprojection',True),('driver_netdisplay','enable',False)]
    saved=[]
    for section,key,value in changes:
        old=data.setdefault(section,{})
        saved.append(dict(section=section,key=key,present=key in old,value=old.get(key),installed=value));old[key]=value
    write_json(STATE,dict(runtime=str(runtime),settings=str(settings),saved=saved))
    try:
        compositor.rename(original);compositor.symlink_to(wrapper)
        write_json(settings,data)
        subprocess.run([str(runtime/'bin/linux64/vrpathreg'),'adddriver',str(PACKAGE/'driver')],check=True)
    except Exception:
        restore();raise
    print('Dedicated VR driver registered; compositor wrapper installed with original backed up.')

def restore():
    require_stopped()
    if not STATE.exists():print('No NetDisplay VR integration installed');return
    state=json.loads(STATE.read_text());runtime=Path(state['runtime']);p=runtime/'bin/linux64/vrcompositor';original=p.with_name('vrcompositor.netdisplay-original')
    if p.is_symlink() and p.resolve()==PACKAGE/'compositor-wrapper':
        p.unlink();original.rename(p)
    elif original.exists():
        raise RuntimeError('SteamVR compositor was replaced externally; original backup retained for manual review')
    settings=Path(state['settings']);data=json.loads(settings.read_text())
    for item in state['saved']:
        section=data.setdefault(item['section'],{})
        if section.get(item['key'])==item['installed']:
            if item['present']:section[item['key']]=item['value']
            else:section.pop(item['key'],None)
    write_json(settings,data)
    subprocess.run([str(runtime/'bin/linux64/vrpathreg'),'removedriver',str(PACKAGE/'driver')],check=True)
    STATE.unlink();print('Original SteamVR compositor and changed settings restored.')

def run():
    config=json.loads(CONFIG.read_text())['backend']
    runtime=os.environ.get('XDG_RUNTIME_DIR')
    if not runtime:raise RuntimeError('XDG_RUNTIME_DIR is required')
    endpoint=Path(runtime)/'netdisplay-vr-ipc'
    args=[str(PACKAGE/'bin/netdisplay-vr-server')]+[str(config[k]) for k in ['bind','peer','control_port','video_port','width','height','fps','key_file','auth','encoder']]+[str(endpoint)]
    os.environ['NETDISPLAY_VR_QP'] = str(config.get('qp',28))
    os.execv(args[0],args)

def status():
    runtime,settings=paths()
    print('Configured:',CONFIG.exists(),'Installed:',STATE.exists())
    for name in ['netdisplay-vr-server','vrserver','vrcompositor']:
        subprocess.run(['pgrep','-a','-f',f'/{name}( |$)'])
    for name in ['vrserver.txt','vrcompositor.txt']:
        p=runtime.parents[2]/'logs'/name
        if p.exists():
            lines=[s for s in p.read_text(errors='replace').splitlines() if any(k in s for k in ['netdisplay','NDVR','chaperone','CannotDRM','VR requires','Failed to initialize','Compositor initialized'])]
            print(name+':\n'+'\n'.join(lines[-8:]))

def tune(a):
    """Update existing paired setup without re-entering or copying its secrets."""
    require_stopped()
    config=json.loads(CONFIG.read_text())
    path=PACKAGE/'driver/resources/settings/default.vrsettings'
    settings=json.loads(path.read_text())
    hmd=settings['driver_netdisplay_vr']; backend=config['backend']
    for key in ['width','height','fps','qp','encoder']:
        value=getattr(a,key,None)
        if value is not None: backend[key]=value
    if backend['width'] % 2 or backend['height'] % 2 or not 2 <= backend['width'] <= 4096 or not 2 <= backend['height'] <= 2160 or not 30 <= backend['fps'] <= 120:
        raise ValueError('Invalid dimensions or frame rate')
    if not 0 <= backend.get('qp',28) <= 51: raise ValueError('QP must be 0..51')
    if a.head_height is not None: hmd['headHeight']=a.head_height
    if not 0.3 <= hmd.get('headHeight',1.6) <= 2.5: raise ValueError('head-height must be 0.3..2.5 metres')
    hmd.update(width=backend['width'],height=backend['height'],frequency=float(backend['fps']))
    config['steamvr_hmd_init_config'].update(eye_resolution_width=backend['width']//2,eye_resolution_height=backend['height'],refresh_rate=backend['fps'])
    write_json(CONFIG,config);write_json(path,settings)
    print(f"Updated: {backend['width']}x{backend['height']}@{backend['fps']} {backend['encoder']} QP={backend.get('qp',28)}; height={hmd.get('headHeight',1.6)} m")
    print('Restart the VR backend as well as SteamVR; select the same video mode on iOS.')
    try:
        _,globalpath=paths()
        override=json.loads(globalpath.read_text()).get('driver_netdisplay_vr',{})
        conflicts=[k for k in ['width','height','frequency','headHeight','motionPort','bindAddress','peerAddress'] if k in override and override[k] != hmd.get(k)]
        if conflicts: print('WARNING: SteamVR global overrides disagree for: '+', '.join(conflicts)+'. Review '+str(globalpath))
    except (OSError,ValueError): pass

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='command',required=True)
    c=sub.add_parser('configure');c.add_argument('--bind',default='0.0.0.0');c.add_argument('--peer',required=True)
    c.add_argument('--control-port',type=int,default=5021);c.add_argument('--video-port',type=int,default=5020);c.add_argument('--motion-port',type=int,default=5010)
    c.add_argument('--width',type=int,default=1920);c.add_argument('--height',type=int,default=1080);c.add_argument('--fps',type=int,default=60)
    c.add_argument('--video-key-file',required=True);c.add_argument('--motion-token-file',required=True);c.add_argument('--auth',choices=['key','password'],default='key');c.add_argument('--encoder',choices=['h264_nvenc','hevc_nvenc','libx264'],default='hevc_nvenc')
    c.add_argument('--qp',type=int,default=28);c.add_argument('--head-height',type=float,default=1.6)
    for command in ['install','restore','run','status','launch']:sub.add_parser(command)
    t=sub.add_parser('tune')
    for key in ['width','height','fps','qp']:t.add_argument('--'+key,type=int)
    t.add_argument('--head-height',type=float)
    t.add_argument('--encoder',choices=['h264_nvenc','hevc_nvenc','libx264'])
    d=sub.add_parser('diagnose');d.add_argument('--openvr-library')
    xr=sub.add_parser('openxr');xr.add_argument('application',nargs=argparse.REMAINDER)
    a=parser.parse_args()
    try:
        if a.command=='configure':configure(a)
        elif a.command=='install':install()
        elif a.command=='restore':restore()
        elif a.command=='run':run()
        elif a.command=='status':status()
        elif a.command=='tune':tune(a)
        elif a.command=='diagnose':
            probe=PACKAGE/'bin/netdisplay-vr-diagnose'
            command=[str(probe)]
            if a.openvr_library:command.append(str(Path(a.openvr_library).expanduser()))
            result=subprocess.run(command)
            return result.returncode
        elif a.command=='openxr':
            if not a.application:raise RuntimeError('Usage: netdisplay-vr openxr APPLICATION [ARGS...]')
            runtime,_=paths();os.environ['XR_RUNTIME_JSON']=str(runtime/'steamxr_linux64.json')
            os.execvp(a.application[0],a.application)
        elif a.command=='launch':
            if not STATE.exists():raise RuntimeError('Run install first')
            subprocess.run(['steam','steam://rungameid/250820'],check=True)
    except (OSError,ValueError,RuntimeError,subprocess.CalledProcessError) as e:
        print(f'netdisplay-vr: {e}',file=sys.stderr);return 1
    return 0
if __name__=='__main__':sys.exit(main())
