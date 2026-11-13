"""Build-time UID2001 content verification only: no Wine, prefix, game or device access."""
import hashlib
import json
import os
from pathlib import Path
import stat

ROOTS = tuple(Path('/home/bazzoozza/.local/share/lutris/runtime') / p
              for p in ('dxvk/v2.6.2', 'vkd3d/v2.14', 'd3d_extras/v2'))


def main():
    assert os.getuid() == os.getgid() == 2001
    raw = Path('/tmp/runtime-manifest.json').read_bytes()
    manifest = json.loads(raw)
    assert manifest['roots'] == [str(p) for p in ROOTS]
    expected = {r['path']: r for r in manifest['guest']}
    actual = set()
    for root in ROOTS:
        for directory, dirs, files in os.walk(root, followlinks=False):
            actual.add(directory)
            actual.update(str(Path(directory) / name) for name in files)
            actual.update(str(Path(directory) / name) for name in dirs if (Path(directory) / name).is_symlink())
    assert actual == set(expected)
    for name, row in expected.items():
        path = Path(name)
        root = next(root for root in ROOTS if path.is_relative_to(root))
        s = path.lstat()
        assert s.st_uid == s.st_gid == 0 and path.resolve(strict=True).is_relative_to(root)
        if row['type'] == 'symlink':
            assert stat.S_ISLNK(s.st_mode) and os.readlink(path) == row['target']
        else:
            assert stat.S_IMODE(s.st_mode) == row['mode'] and s.st_mode & 0o022 == 0
            assert os.access(path, os.R_OK)
            if row['type'] == 'directory':
                assert stat.S_ISDIR(s.st_mode) and os.access(path, os.X_OK)
            else:
                assert stat.S_ISREG(s.st_mode) and s.st_size == row['bytes']
                assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256']
        for ancestor in path.parents:
            if ancestor == Path('/'): break
            assert os.access(ancestor, os.X_OK)
    print('MWOW_RUNTIME_CONTENT ' + json.dumps({'uid': os.getuid(), 'gid': os.getgid(),
          'manifest_sha256': hashlib.sha256(raw).hexdigest(), 'paths': len(actual),
          'files': sum(r['type'] == 'file' for r in expected.values()), 'hashes_match': True,
          'root_owned': True, 'uid2001_readable': True, 'no_group_other_write': True}, sort_keys=True))


if __name__ == '__main__':
    main()
