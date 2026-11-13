import hashlib
import json
from pathlib import Path
import unittest

IMAGE = Path(__file__).resolve().parents[1]


class Recipe(unittest.TestCase):
    def test_bounded_parallel_downloads_in_options(self):
        recipe = (IMAGE / 'Dockerfile').read_text()
        options = recipe.split("'[options]'", 1)[1].split("'[core]'", 1)[0]
        self.assertEqual(options.count("'ParallelDownloads = 5'"), 1)
        self.assertEqual(recipe.count('ParallelDownloads'), 1)
        self.assertNotIn('DisableDownloadTimeout', recipe)

    def test_oci_and_package_evidence(self):
        raw = (IMAGE / 'evidence/base-amd64-manifest.json').read_bytes()
        digest = 'sha256:' + hashlib.sha256(raw).hexdigest()
        index = json.loads((IMAGE / 'evidence/base-index.json').read_text())
        selected = [v for v in index['manifests'] if v['platform'].get('architecture') == 'amd64'
                    and v['platform'].get('os') == 'linux']
        self.assertEqual(len(selected), 1)
        self.assertEqual(selected[0]['digest'], digest)
        self.assertIn('archlinux@' + digest, (IMAGE / 'Dockerfile').read_text())
        lock = json.loads((IMAGE / 'packages.lock.json').read_text())
        for repo, meta in lock['databases'].items():
            self.assertIn(meta['sha256'] + '  ' + repo + '.db', (IMAGE / 'package-db.sha256').read_text())
        for pin in (IMAGE / 'packages.requested').read_text().splitlines():
            name, version = pin.split('=', 1)
            self.assertEqual(version, lock['selected_packages'][name]['version'])
        self.assertIn('sha256sum -c /opt/wolf/package-db.sha256', (IMAGE / 'Dockerfile').read_text())

    def test_python_sources_parse_without_bytecode(self):
        for path in [*IMAGE.glob('*.py'), IMAGE / 'approved-entrypoint', *IMAGE.joinpath('tests').glob('*.py')]:
            compile(path.read_text(), str(path), 'exec')


if __name__ == '__main__':
    unittest.main(verbosity=2)
