"""Fixed MWOW-9 policy and init. No caller-selected executable or runtime test mode."""
import fcntl
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import ctypes

UID = 2001
HOME = '/home/retro'
PREFIX = HOME + '/prefix'
RUNTIME = '/run/user/2001'
PULSE = RUNTIME + '/pulse-socket'
BATTLE_NET = PREFIX + '/drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe'
WINE = '/opt/wine/bin/wine'
SERVER = '/opt/wine/bin/wineserver'
LOCK = '.mwow9-wine-session.lock'  # NOT outer launcher/prefix-<dev>-<ino>.lock


class Refused(Exception):
    pass


def require(ok):
    if not ok:
        raise Refused('fixed guest policy refused')


def authority(status, uid, gid, groups):
    """Validate real /proc facts; bounding bits intentionally are not effective rights."""
    require(uid == UID and gid == UID and 0 not in groups)
    rows = dict(line.split(':', 1) for line in status.splitlines() if ':' in line)
    try:
        require([int(x) for x in rows['Uid'].split()] == [UID] * 4)
        require([int(x) for x in rows['Gid'].split()] == [UID] * 4)
        for key in ('CapEff', 'CapPrm', 'CapInh', 'CapAmb'):
            require(bool(re.fullmatch(r'[0-9a-fA-F]{1,16}', rows[key].strip())))
            require(int(rows[key], 16) == 0)
        require(rows['NoNewPrivs'].strip() == '1')
    except (KeyError, ValueError):
        raise Refused('missing authority facts') from None


def check_authority():
    authority(Path('/proc/self/status').read_text(), os.getuid(), os.getgid(), os.getgroups())


def private_socket(path, info):
    require(path in (PULSE,) or bool(re.fullmatch(RUNTIME + r'/wayland-[A-Za-z0-9_-]{0,88}', path)))
    require(stat.S_ISSOCK(info.st_mode) and info.st_uid == UID)


def inputs(env, args):
    require(not args)
    require(env.get('HOME') == HOME and env.get('WINEPREFIX') == PREFIX)
    require(env.get('XDG_RUNTIME_DIR') == RUNTIME and env.get('PULSE_SERVER') == PULSE)
    name = env.get('WAYLAND_DISPLAY', '')
    require(len(name) <= 96 and bool(re.fullmatch(r'wayland-[A-Za-z0-9_-]*', name)))
    require(not env.get('DISPLAY'))
    # Match native's client-negotiated fields; never interpolate into a shell.
    values = []
    for key, low, high in [('GAMESCOPE_WIDTH', 320, 7680),
                           ('GAMESCOPE_HEIGHT', 240, 4320),
                           ('GAMESCOPE_REFRESH', 1, 240)]:
        value = env.get(key, '')
        require(bool(re.fullmatch(r'[1-9][0-9]{0,3}', value)))
        require(low <= int(value) <= high)
        values.append(value)
    sink = env.get('PULSE_SINK', '')
    require(bool(re.fullmatch(r'virtual_sink_[A-Za-z0-9_-]{1,96}', sink)))
    clean = {'PATH': '/usr/bin', 'HOME': HOME, 'WINEPREFIX': PREFIX,
             'WINEARCH': 'win64', 'XDG_RUNTIME_DIR': RUNTIME,
             'PULSE_SERVER': 'unix:' + PULSE, 'PULSE_SINK': sink, 'WAYLAND_DISPLAY': name,
             'LANG': 'C.UTF-8', 'WINEDEBUG': '-all'}
    return clean, values


def gamescope_command(values):
    w, h, r = values
    return ['/usr/bin/gamescope', '--backend', 'wayland', '-w', w, '-h', h,
            '-W', w, '-H', h, '-r', r, '--', '/usr/bin/python3', '-I',
            '/opt/wolf/wine-session.py']


def acquire_prefix(directory):
    """Lock the same inode for every invocation; never unlink/replace a busy lease."""
    root = os.open(directory, os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        s = os.fstat(root)
        require(s.st_uid == UID and s.st_mode & 0o022 == 0)
        fd = os.open(LOCK, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC,
                     0o600, dir_fd=root)
    finally:
        os.close(root)
    try:
        s = os.fstat(fd)
        require(stat.S_ISREG(s.st_mode) and s.st_uid == UID
                and s.st_nlink == 1 and s.st_mode & 0o077 == 0)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd
    except BaseException:
        os.close(fd)
        raise


def subreaper():
    libc = ctypes.CDLL(None, use_errno=True)
    require(libc.prctl(36, 1, 0, 0, 0) == 0)  # PR_SET_CHILD_SUBREAPER, needs no capability


def wait_tree(leader):
    """Wait for adopted descendants too, including double-forked Wine/wineserver."""
    result = 1
    while True:
        try:
            pid, status = os.waitpid(-1, 0)
        except ChildProcessError:
            return result
        if pid == leader:
            result = os.waitstatus_to_exitcode(status)


STOP = None


def forward(sig, _frame):
    global STOP
    STOP = sig
    # Only our private PID namespace descendants. Never match host names/prefixes.
    pending = [os.getpid()]
    targets = []
    while pending:
        parent = pending.pop()
        try:
            children = Path(f'/proc/{parent}/task/{parent}/children').read_text().split()
        except OSError:
            continue
        for child in children:
            pid = int(child)
            targets.append(pid)
            pending.append(pid)
    for pid in reversed(targets):
        try:
            os.kill(pid, sig)
        except (ProcessLookupError, PermissionError):
            pass


def wine_session(env, args, run=subprocess.run):
    require(not args)
    check_authority()
    require(env.get('HOME') == HOME and env.get('WINEPREFIX') == PREFIX)
    require(env.get('XDG_RUNTIME_DIR') == RUNTIME)
    require(env.get('PULSE_SERVER') == 'unix:' + PULSE)
    display = env.get('DISPLAY', '')
    require(bool(re.fullmatch(r':[0-9]{1,4}(\.[0-9]{1,2})?', display)))
    sink = env.get('PULSE_SINK', '')
    require(bool(re.fullmatch(r'virtual_sink_[A-Za-z0-9_-]{1,96}', sink)))
    # Gamescope supplies only its nested Xwayland DISPLAY. Never pass host variables.
    clean = {'PATH': '/usr/bin', 'HOME': HOME, 'WINEPREFIX': PREFIX,
             'WINEARCH': 'win64', 'XDG_RUNTIME_DIR': RUNTIME,
             'PULSE_SERVER': 'unix:' + PULSE, 'PULSE_SINK': sink, 'DISPLAY': display,
             'LANG': 'C.UTF-8', 'WINEDEBUG': '-all',
             'LD_LIBRARY_PATH': '/opt/wine/lib:/opt/wine/lib64'}
    result = run([WINE, BATTLE_NET], env=clean, check=False).returncode
    # -w waits for THIS WINEPREFIX server; launcher exit is not game exit.
    server = run([SERVER, '-w'], env=clean, check=False).returncode
    return result or server


def entry(env, args):
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, forward)
    require(os.getpid() == 1)  # This Python process is the guest init, no external init.
    check_authority()
    clean, values = inputs(env, args)
    runtime = os.lstat(RUNTIME)
    require(stat.S_ISDIR(runtime.st_mode) and runtime.st_uid == UID
            and runtime.st_mode & 0o777 == 0o700)
    for path in (PULSE, RUNTIME + '/' + clean['WAYLAND_DISPLAY']):
        private_socket(path, os.lstat(path))  # no fallback, socket creation or listener
    require(Path(PREFIX).resolve() == Path(PREFIX))
    require(Path(BATTLE_NET).is_file())
    os.umask(0o077)
    lease = acquire_prefix(PREFIX)
    try:
        subreaper()
        require(STOP is None)
        child = subprocess.Popen(gamescope_command(values), env=clean, cwd=HOME)
        if STOP is not None:
            forward(STOP, None)
        result = wait_tree(child.pid)
        child.returncode = result
        return result if result >= 0 else 128 - result
    finally:
        os.close(lease)  # Only after every descendant is gone, including orphans.
