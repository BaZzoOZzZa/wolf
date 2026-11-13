#!/usr/bin/env python3
"""Package only the three existing versioned Lutris software trees, never a prefix/home."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import stat
import tarfile

ROOTS = tuple(Path('/home/bazzoozza/.local/share/lutris/runtime') / p
              for p in ('dxvk/v2.6.2', 'vkd3d/v2.14', 'd3d_extras/v2'))
OUTPUT_ROOT = Path('/run/media/bazzoozza/Games/bb-targets/wolf/isolated-image-source')


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def signature(s):
    return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)


def open_regular(path, expected):
    flags = os.O_RDONLY | os.O_NOFOLLOW
    if expected.st_uid == os.geteuid():
        flags |= os.O_NOATIME
    fd = os.open(path, flags)
    actual = os.fstat(fd)
    require(stat.S_ISREG(actual.st_mode) and signature(actual) == signature(expected), 'source changed')
    return os.fdopen(fd, 'rb')


def inventory():
    source, guest, paths = [], [], {}
    hardlinks = collections.Counter()
    for root in ROOTS:
        require(root.resolve(strict=True) == root and root.is_dir(), 'software root alias refused')
        for directory, dirs, files in os.walk(root, followlinks=False):
            for path in [Path(directory), *(Path(directory) / n for n in sorted(files)),
                         *(Path(directory) / n for n in sorted(dirs) if (Path(directory) / n).is_symlink())]:
                require(path not in paths, 'duplicate inventory path')
                s = path.lstat()
                resolved = path.resolve(strict=True)
                require(resolved.is_relative_to(root), 'escaping software symlink')
                item = {'path': str(path), 'mode': stat.S_IMODE(s.st_mode), 'uid': s.st_uid,
                        'gid': s.st_gid, 'bytes': s.st_size, 'dev': s.st_dev, 'ino': s.st_ino,
                        'nlink': s.st_nlink, 'mtime_ns': s.st_mtime_ns, 'ctime_ns': s.st_ctime_ns,
                        'resolved': str(resolved)}
                target = {'path': str(path), 'uid': 0, 'gid': 0}
                if stat.S_ISREG(s.st_mode):
                    require(s.st_size <= 256 * 1024**2, 'software file too large')
                    h = hashlib.sha256()
                    with open_regular(path, s) as f:
                        while block := f.read(1024 * 1024):
                            h.update(block)
                        require(signature(os.fstat(f.fileno())) == signature(s), 'source changed while hashing')
                    item.update(type='file', sha256=h.hexdigest())
                    target.update(type='file', bytes=s.st_size, sha256=h.hexdigest(),
                                  mode=0o555 if s.st_mode & 0o111 else 0o444)
                    hardlinks[(s.st_dev, s.st_ino)] += 1
                elif stat.S_ISDIR(s.st_mode):
                    item['type'] = 'directory'
                    target.update(type='directory', mode=0o555)
                elif stat.S_ISLNK(s.st_mode):
                    require(resolved.is_file() or resolved.is_dir(), 'non-software symlink target')
                    item.update(type='symlink', target=os.readlink(path))
                    target.update(type='symlink', target=item['target'], mode=0o777)
                else:
                    raise RuntimeError('special source file refused')
                paths[path] = s
                source.append(item)
                guest.append(target)
    require(len(source) <= 4096 and sum(r['bytes'] for r in guest if r['type'] == 'file') <= 1024**3,
            'software bundle budget exceeded')
    for path, s in paths.items():
        if stat.S_ISREG(s.st_mode):
            require(hardlinks[(s.st_dev, s.st_ino)] == s.st_nlink, 'external hardlink refused')
    return sorted(source, key=lambda r: r['path']), sorted(guest, key=lambda r: r['path']), paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--context', required=True, type=Path)
    args = parser.parse_args()
    context = args.context
    require(context.is_absolute() and context.parent.resolve() == context.parent
            and context.is_relative_to(OUTPUT_ROOT) and not context.exists(), 'fresh approved context required')
    source, guest, paths = inventory()  # every file hashed before any copy
    context.mkdir(mode=0o700, parents=True)
    manifest = {'schema': 1, 'roots': [str(p) for p in ROOTS], 'source': source, 'guest': guest,
                'scope': 'existing installed software, upstream release/signature provenance not asserted'}
    (context / 'runtime-manifest.json').write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    with tarfile.open(context / 'runtime-software.tar', 'x', format=tarfile.PAX_FORMAT) as archive:
        for row in guest:
            path = Path(row['path'])
            require(signature(path.lstat()) == signature(paths[path]), 'source changed before copy')
            info = tarfile.TarInfo(str(path).lstrip('/'))
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mode, info.mtime = row['mode'], 0
            if row['type'] == 'directory':
                info.type = tarfile.DIRTYPE
                archive.addfile(info)
            elif row['type'] == 'symlink':
                require(os.readlink(path) == row['target'], 'symlink changed before copy')
                info.type, info.linkname = tarfile.SYMTYPE, row['target']
                archive.addfile(info)
            else:
                info.size = row['bytes']
                with open_regular(path, paths[path]) as f:
                    archive.addfile(info, f)
                    require(signature(os.fstat(f.fileno())) == signature(paths[path]), 'source changed during copy')
    # Verify copied bytes against the pre-copy inventory, not a second inferred manifest.
    expected = {r['path'].lstrip('/'): r for r in guest}
    with tarfile.open(context / 'runtime-software.tar') as archive:
        members = archive.getmembers()
        require({m.name for m in members} == set(expected), 'archive path set differs')
        for m in members:
            row = expected[m.name]
            if m.isfile():
                require(hashlib.sha256(archive.extractfile(m).read()).hexdigest() == row['sha256'],
                        'copied bytes differ from inventory')
    for name in ('Dockerfile', 'verify.py', '.dockerignore'):
        (context / name).write_bytes((Path(__file__).parent / name).read_bytes())
    print(json.dumps({'context': str(context), 'files': sum(r['type'] == 'file' for r in guest),
                      'symlinks': sum(r['type'] == 'symlink' for r in guest),
                      'software_bytes': sum(r.get('bytes', 0) for r in guest),
                      'manifest_sha256': hashlib.sha256((context / 'runtime-manifest.json').read_bytes()).hexdigest(),
                      'archive_sha256': hashlib.sha256((context / 'runtime-software.tar').read_bytes()).hexdigest()}, indent=2))


if __name__ == '__main__':
    main()
