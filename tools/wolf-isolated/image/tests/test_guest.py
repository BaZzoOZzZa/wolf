import importlib.util
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

IMAGE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('guest', IMAGE / 'guest.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
STATUS = '\n'.join(['Uid: 2001 2001 2001 2001', 'Gid: 2001 2001 2001 2001',
                   'CapEff: 0000', 'CapPrm: 0000', 'CapInh: 0000', 'CapAmb: 0000',
                   'CapBnd: 0000000000003000', 'NoNewPrivs: 1'])
ENV = {'HOME': g.HOME, 'WINEPREFIX': g.PREFIX, 'XDG_RUNTIME_DIR': g.RUNTIME,
       'PULSE_SERVER': g.PULSE, 'WAYLAND_DISPLAY': 'wayland-client_123',
       'PULSE_SINK': 'virtual_sink_123',
       'GAMESCOPE_WIDTH': '1920', 'GAMESCOPE_HEIGHT': '1080', 'GAMESCOPE_REFRESH': '60'}


class Policy(unittest.TestCase):
    def test_authority(self):
        g.authority(STATUS, 2001, 2001, [2001, 989, 2002])
        for status, uid, gid, groups in [(STATUS, 0, 2001, []),
                                        (STATUS, 2001, 0, []),
                                        (STATUS, 2001, 2001, [0]),
                                        (STATUS.replace('2001 2001 2001 2001', '2001 0 2001 2001'), 2001, 2001, [])]:
            with self.subTest(status=status, uid=uid, gid=gid, groups=groups):
                with self.assertRaises(g.Refused):
                    g.authority(status, uid, gid, groups)
        for key in ['CapEff', 'CapPrm', 'CapInh', 'CapAmb']:
            with self.subTest(key=key):
                with self.assertRaises(g.Refused):
                    g.authority(STATUS.replace(key + ': 0000', key + ': 0001'), 2001, 2001, [])
                with self.assertRaises(g.Refused):
                    g.authority(STATUS.replace(key + ': 0000', ''), 2001, 2001, [])
        with self.assertRaises(g.Refused):
            g.authority(STATUS.replace('NoNewPrivs: 1', 'NoNewPrivs: 0'), 2001, 2001, [])

    def test_inputs_and_quoting(self):
        env, dims = g.inputs(dict(ENV, LD_PRELOAD='evil', PYTHONPATH='evil', WINELOADER='evil'), [])
        self.assertNotIn('LD_PRELOAD', env)
        self.assertNotIn('PYTHONPATH', env)
        self.assertNotIn('WINELOADER', env)
        self.assertEqual(g.gamescope_command(dims)[-1], '/opt/wolf/wine-session.py')
        for key, bad in [('HOME', '/root'), ('WINEPREFIX', '/original'),
                         ('XDG_RUNTIME_DIR', '/run/user/1000'), ('PULSE_SERVER', 'tcp:host'),
                         ('WAYLAND_DISPLAY', '../wayland-1'), ('WAYLAND_DISPLAY', 'wayland-a;id'),
                         ('WAYLAND_DISPLAY', 'wayland-' + 'x' * 90), ('DISPLAY', ':0'),
                         ('GAMESCOPE_WIDTH', '1920;id'), ('GAMESCOPE_HEIGHT', '99999'),
                         ('GAMESCOPE_REFRESH', '0'), ('PULSE_SINK', 'default')]:
            with self.subTest(key=key, bad=bad):
                with self.assertRaises(g.Refused):
                    g.inputs(dict(ENV, **{key: bad}), [])
        for args in [['bash'], ['--help'], ['-c', 'id']]:
            with self.assertRaises(g.Refused):
                g.inputs(ENV, args)

    def test_socket_facts(self):
        good = SimpleNamespace(st_mode=stat.S_IFSOCK | 0o600, st_uid=2001)
        g.private_socket(g.RUNTIME + '/wayland-client_123', good)
        g.private_socket(g.PULSE, good)
        for path, info in [('/tmp/socket', good), (g.PULSE, SimpleNamespace(st_mode=stat.S_IFREG, st_uid=2001)),
                           (g.PULSE, SimpleNamespace(st_mode=stat.S_IFSOCK, st_uid=1000)),
                           (g.PULSE, SimpleNamespace(st_mode=stat.S_IFLNK, st_uid=2001))]:
            with self.assertRaises(g.Refused):
                g.private_socket(path, info)

    def test_wine_adapter_fixed_argv_and_server_wait(self):
        clean, _ = g.inputs(ENV, [])
        clean['DISPLAY'] = ':3.0'
        calls = []
        def fake(argv, **kw):
            calls.append((argv, kw))
            return SimpleNamespace(returncode=7 if len(calls) == 1 else 0)
        with patch.object(g, 'check_authority'):
            self.assertEqual(g.wine_session(clean, [], fake), 7)
            with self.assertRaises(g.Refused):
                g.wine_session(dict(clean, DISPLAY='host:0'), [], fake)
            with self.assertRaises(g.Refused):
                g.wine_session(clean, ['other.exe'], fake)
        self.assertEqual([x[0] for x in calls], [[g.WINE, g.BATTLE_NET], [g.SERVER, '-w']])
        self.assertEqual(calls[0][0][1], g.BATTLE_NET)  # spaces remain one argv element
        self.assertNotIn('WAYLAND_DISPLAY', calls[0][1]['env'])

    def test_guest_lease_conflict_symlink_and_release(self):
        # Real flock on synthetic storage; metadata facts simulate provisioned UID only.
        real = os.fstat
        def facts(fd):
            s = real(fd)
            return SimpleNamespace(st_uid=2001, st_mode=s.st_mode, st_nlink=s.st_nlink)
        with tempfile.TemporaryDirectory(prefix='mwow9-', dir=os.environ['MWOW9_TEST_OUTPUT']) as d:
            with patch.object(g.os, 'fstat', side_effect=facts):
                fd = g.acquire_prefix(d)
                with self.assertRaises(BlockingIOError):
                    g.acquire_prefix(d)
                os.close(fd)
                fd = g.acquire_prefix(d)
                os.close(fd)
                os.unlink(Path(d) / g.LOCK)
                (Path(d) / g.LOCK).symlink_to('elsewhere')
                with self.assertRaises(OSError):
                    g.acquire_prefix(d)
                with self.assertRaises(g.Refused):
                    with patch.object(g.os, 'fstat', return_value=SimpleNamespace(st_uid=0, st_mode=0o700)):
                        g.acquire_prefix(d)

    def test_subreaper_keeps_lease_after_launcher_exit(self):
        # Separate synthetic Python init; fake Wine double-forks. No Wine executable or socket.
        code = '''import importlib.util, os, sys, time, fcntl
s=importlib.util.spec_from_file_location('g',sys.argv[1]);g=importlib.util.module_from_spec(s);s.loader.exec_module(g)
g.subreaper()
f=open(sys.argv[2],'w');fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
p=os.fork()
if p==0:
 q=os.fork()
 if q==0:
  os.close(f.fileno())
  time.sleep(.1)
  other=open(sys.argv[2],'r+')
  try:
   fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
   os._exit(91)
  except BlockingIOError:
   pass
  time.sleep(.1)
  open(sys.argv[3],'w').close()
 os._exit(0)
assert g.wait_tree(p)==0
assert os.path.exists(sys.argv[3])
f.close()
'''
        with tempfile.TemporaryDirectory(prefix='mwow9-', dir=os.environ['MWOW9_TEST_OUTPUT']) as d:
            result = subprocess.run([sys.executable, '-I', '-B', '-c', code, str(IMAGE / 'guest.py'),
                                     d + '/lease', d + '/child-finished'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            import fcntl
            with open(d + '/lease') as released:
                fcntl.flock(released, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_signal_routes_only_descendant_pids(self):
        tree = {'/proc/10/task/10/children': '20', '/proc/20/task/20/children': '30',
                '/proc/30/task/30/children': ''}
        with patch.object(g.os, 'getpid', return_value=10), \
             patch.object(g.Path, 'read_text', lambda p: tree[str(p)]), \
             patch.object(g.os, 'kill') as kill:
            g.forward(15, None)
            self.assertEqual([c.args for c in kill.call_args_list], [(30, 15), (20, 15)])
        g.STOP = None


if __name__ == '__main__':
    unittest.main(verbosity=2)
