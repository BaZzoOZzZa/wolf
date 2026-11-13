#!/usr/bin/python3
"""Launch at most one WoW in an already running, user-selected Moonlight session."""
import json
from pathlib import Path
import subprocess
import sys

assert sys.argv[1:] in ([], ['--dx11'])
for p in Path('/proc').iterdir():
    if not p.name.isdecimal():
        continue
    try:
        name = (p / 'comm').read_text().strip().lower()
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        continue
    if name == 'wow.exe':
        print('WoW already running on this host; no second instance started')
        raise SystemExit(0)
docker = ['docker', '--host', 'unix:///run/docker.sock']
image = 'sha256:2218657d347e907c4a899f4a2c82f6ae25cfd7f52cb78166f57d24bed4f54745'
rows = subprocess.check_output(docker + ['ps', '--no-trunc', '--filter', 'ancestor=' + image, '--format', '{{json .}}'], text=True)
rows = [json.loads(s) for s in rows.splitlines()]
rows = [r for r in rows if r['Names'].startswith('WolfWowIsolated_')]
if len(rows) != 1:
    raise SystemExit('Need exactly one live Moonlight WoW session; no container or game started')
code = r'''
import os,re,subprocess,sys
from pathlib import Path
assert os.getuid()==2001
clients=[]
for p in Path('/proc').iterdir():
 if not p.name.isdecimal():continue
 try:name=(p/'comm').read_text().strip()
 except (FileNotFoundError,ProcessLookupError):continue
 if name.lower()=='wow.exe' or any(re.split(r'[/\\]',a)[-1].lower()=='wow.exe' for a in (p/'cmdline').read_bytes().decode(errors='replace').split('\0')):raise SystemExit('WoW already running; no second instance')
 if (name in ('Battle.net.exe','Agent.exe') or name=='Main') and any(re.split(r'[/\\]',a)[-1].lower() in ('battle.net.exe','agent.exe') for a in (p/'cmdline').read_bytes().decode(errors='replace').split('\0')):clients.append(p)
assert clients,'existing Wine session context unavailable'
p=min(clients,key=lambda x:int(x.name))
values=dict(os.fsdecode(x).split('=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
keys=('PATH','HOME','WINEPREFIX','WINEARCH','XDG_RUNTIME_DIR','PULSE_SERVER','PULSE_SINK','DISPLAY','LANG','WINEDEBUG','LD_LIBRARY_PATH','XAUTHORITY')
env={k:values[k] for k in keys if k in values}
env['WINEDLLOVERRIDES']='dxgi,d3d11=n,b'
env['DXVK_LOG_PATH']='/tmp'
env['DXVK_LOG_LEVEL']='info'
assert env['WINEPREFIX']=='/home/retro/prefix' and env['HOME']=='/home/retro'
assert re.fullmatch(r':[0-9]{1,4}(\.[0-9]{1,2})?',env['DISPLAY'])
game=Path('/games/wow/_forever_/Wow.exe');assert game.is_file()
argv=['/opt/wine/bin/wine',str(game)]
if sys.argv[1:]==['--dx11']:argv+=['-d3d11']
with open('/tmp/mwow-direct-game.log','ab',buffering=0) as log:
 child=subprocess.Popen(argv,env=env,cwd=str(game.parent),stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,close_fds=True)
print('WoW launch requested; container PID',child.pid)
'''
r = subprocess.run(docker + ['exec', '--user', '2001', '-i', rows[0]['ID'], '/usr/bin/python3', '-I', '-', *sys.argv[1:]], input=code, text=True)
raise SystemExit(r.returncode)
