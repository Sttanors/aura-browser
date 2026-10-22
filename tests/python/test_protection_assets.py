"""Bundled protection assets must match their recorded upstream snapshots."""
import hashlib,json,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class ProtectionAssets(unittest.TestCase):
    def test_snapshot_hashes(self):
        manifest=json.loads((ROOT/'resources/protection/dependencies.json').read_text())
        for name,source in manifest.items():
            with self.subTest(name=name):
                self.assertTrue(source['url'].startswith('https://'))
                self.assertEqual(hashlib.sha256((ROOT/'resources'/name).read_bytes()).hexdigest(),source['sha256'])
    def test_distribution_has_licenses(self):
        for name in ['darkreader-LICENSE','adblock-LICENSE','easylist-GPL-3.0.txt','NOTICE.txt']:
            self.assertGreater((ROOT/'resources/vendor'/name).stat().st_size,100)

class ReaderAssets(unittest.TestCase):
    def test_pinned_reader_files_and_license(self):
        manifest=json.loads((ROOT/'resources/vendor/readability-lock.json').read_text())
        self.assertEqual(len(manifest['commit']),40)
        for name,source in manifest['files'].items():
            self.assertIn('/'+manifest['commit']+'/',source['url'])
            self.assertEqual(hashlib.sha256((ROOT/'resources/vendor'/name).read_bytes()).hexdigest(),source['sha256'])
        self.assertIn('Apache License',(ROOT/'resources/vendor/READABILITY-LICENSE.md').read_text())

if __name__=='__main__':unittest.main()
