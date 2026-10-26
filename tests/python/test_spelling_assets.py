"""Bundled spelling data must remain reproducible and include its upstream notices."""
import hashlib,json,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class SpellingAssets(unittest.TestCase):
    def test_pinned_dictionaries_and_notices_match_manifest(self):
        folder=ROOT/'resources/Dictionaries'
        manifest=json.loads((folder/'manifest.json').read_text())
        self.assertRegex(manifest['revision'],r'^[0-9a-f]{40}$')
        self.assertEqual(manifest['repository'],'https://chromium.googlesource.com/chromium/deps/hunspell_dictionaries/')
        for name in ('en-US-10-2.bdic','ru-RU-3-0.bdic','README_en_US.txt','README_ru_RU.txt','LICENSE','COPYING'):
            self.assertIn(name,manifest['files'])
        for name,expected in manifest['files'].items():
            self.assertEqual(Path(name).name,name)
            data=(folder/name).read_bytes()
            self.assertEqual(len(data),expected['bytes'])
            self.assertEqual(hashlib.sha256(data).hexdigest(),expected['sha256'])
