"""R41-1: actual entry/wait code, deterministic IO facts, no guest workload."""
import errno
import fcntl
import importlib.util
import os
from pathlib import Path
import signal
import stat
import tempfile
from contextlib import ExitStack
from types import SimpleNamespace
import unittest
from unittest.mock import patch

IMAGE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lifetime_guest', IMAGE / 'guest.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
ENV = {'HOME': g.HOME, 'WINEPREFIX': g.PREFIX, 'XDG_RUNTIME_DIR': g.RUNTIME,
       'PULSE_SERVER': g.PULSE, 'WAYLAND_DISPLAY': 'wayland-client_123',
       'PULSE_SINK': 'virtual_sink_123', 'GAMESCOPE_WIDTH': '1920',
       'GAMESCOPE_HEIGHT': '1080', 'GAMESCOPE_REFRESH': '60'}


class Lifetime(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ['MWOW9_TEST_OUTPUT'], prefix='r41-')
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'lease'
        self.lease = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o600)
        fcntl.flock(self.lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.closed = []
        self.events = []
        self.handlers = {}
        self.real_close = os.close
        self.stack = ExitStack()
        self.addCleanup(self.cleanup_fixture)
        self.addCleanup(self.stack.close)
        g.STOP = None
        if hasattr(g, 'LIFETIME_FAULT'):
            g.LIFETIME_FAULT = False
        def close(fd):
            if fd == self.lease:
                self.events.append('release')
                self.closed.append(fd)
            self.real_close(fd)
        def socket(path):
            return SimpleNamespace(st_mode=(stat.S_IFDIR | 0o700) if path == g.RUNTIME else stat.S_IFSOCK,
                                   st_uid=g.UID)
        patches = [patch.object(g, 'check_authority'), patch.object(g, 'subreaper'),
                   patch.object(g.os, 'getpid', return_value=1),
                   patch.object(g.os, 'lstat', side_effect=socket),
                   patch.object(g.os, 'umask'), patch.object(g.os, 'close', side_effect=close),
                   patch.object(g.Path, 'resolve', lambda p: p),
                   patch.object(g.Path, 'is_file', return_value=True),
                   patch.object(g, 'acquire_prefix', return_value=self.lease),
                   patch.object(g.signal, 'signal', side_effect=lambda s, h: self.handlers.update({s: h}))]
        for p in patches:
            self.stack.enter_context(p)
        self.child = SimpleNamespace(pid=71, returncode=None)

    def cleanup_fixture(self):
        # Test-only bounded cleanup, never a production abandonment mechanism.
        if not self.closed:
            self.real_close(self.lease)

    def held(self):
        self.assertEqual(self.closed, [], self.events)
        os.fstat(self.lease)  # actual fd still open
        with open(self.path) as other:
            with self.assertRaises(BlockingIOError):
                fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def released(self):
        self.assertEqual(self.closed, [self.lease])
        with open(self.path) as other:
            fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertEqual(self.events[-1], 'release')

    def test_prelaunch_failure_releases_without_child(self):
        with patch.object(g.subprocess, 'Popen', side_effect=OSError('exec failed')) as spawn:
            with self.assertRaises(OSError):
                g.entry(ENV, [])
            spawn.assert_called_once()
        self.released()

    def exercise_fault(self, kind):
        step = 0
        def waitpid(pid, flags):
            nonlocal step
            self.assertEqual((pid, flags), (-1, 0))
            self.held()
            step += 1
            if step == 1:
                self.events.append(kind + '-fault')
                if kind == 'propagated-handler':
                    with patch.object(g.Path, 'read_text', side_effect=RuntimeError('handler traversal fault')):
                        g.forward(signal.SIGTERM, None)
                raise OSError(errno.EIO, 'uncertain waitpid')
            if step == 2:
                self.events.append('leader-reaped')
                return (71, 0)
            if step == 3:
                self.events.append('late-descendant-reaped')
                return (99, 0)
            if step == 4:
                self.events.append('positive-ECHILD')
                raise ChildProcessError(errno.ECHILD, 'no children')
            self.fail('unexpected wait retry')
        original_wait = g.wait_tree
        attempts = 0
        def wait_tree(leader):
            nonlocal attempts
            attempts += 1
            self.held()
            if kind == 'wait-tree' and attempts == 1:
                self.events.append('wait-tree-fault')
                raise RuntimeError('wait wrapper fault')
            return original_wait(leader)
        with patch.object(g.subprocess, 'Popen', return_value=self.child) as spawn, \
             patch.object(g.os, 'waitpid', side_effect=waitpid), \
             patch.object(g, 'wait_tree', side_effect=wait_tree):
            result = g.entry(ENV, [])
            spawn.assert_called_once()
        self.assertEqual(step, 4)
        self.assertNotEqual(result, 0)  # drain succeeds, uncertainty is still failure
        self.assertLess(self.events.index('positive-ECHILD'), self.events.index('release'))
        self.released()

    def test_postspawn_waitpid_fault_holds_owner_until_positive_drain(self):
        self.exercise_fault('waitpid')

    def test_postspawn_wait_tree_fault_holds_owner_until_positive_drain(self):
        self.exercise_fault('wait-tree')

    def test_postspawn_propagated_handler_fault_holds_owner_until_positive_drain(self):
        self.exercise_fault('propagated-handler')

    def test_installed_handler_fault_does_not_escape_owned_wait(self):
        step = 0
        def waitpid(*args):
            nonlocal step
            self.held()
            step += 1
            if step == 1:
                with patch.object(g.Path, 'read_text', side_effect=RuntimeError('handler fault')):
                    self.handlers[signal.SIGTERM](signal.SIGTERM, None)
                return (71, 0)
            if step == 2:
                return (99, 0)
            self.events.append('positive-ECHILD')
            raise ChildProcessError(errno.ECHILD, 'no children')
        with patch.object(g.subprocess, 'Popen', return_value=self.child), \
             patch.object(g.os, 'waitpid', side_effect=waitpid):
            result = g.entry(ENV, [])
        self.assertNotEqual(result, 0)
        self.released()

    def test_failed_wineserver_wait_does_not_release_init_lease(self):
        clean, _ = g.inputs(ENV, [])
        clean['DISPLAY'] = ':3'
        calls = []
        def wine(argv, **kwargs):
            self.held()
            calls.append(argv)
            return SimpleNamespace(returncode=0 if len(calls) == 1 else 9)
        step = 0
        def waitpid(*args):
            nonlocal step
            self.held()
            step += 1
            if step == 1:
                self.assertEqual(g.wine_session(clean, [], wine), 9)
                return (71, 9 << 8)
            if step == 2:
                self.events.append('late-descendant-reaped')
                return (99, 0)
            self.events.append('positive-ECHILD')
            raise ChildProcessError(errno.ECHILD, 'no children')
        with patch.object(g.subprocess, 'Popen', return_value=self.child), \
             patch.object(g.os, 'waitpid', side_effect=waitpid):
            self.assertEqual(g.entry(ENV, []), 9)
        self.assertEqual(calls, [[g.WINE, g.BATTLE_NET], [g.SERVER, '-w']])
        self.released()
