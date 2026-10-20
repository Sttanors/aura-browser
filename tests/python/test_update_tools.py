"""Release trust and Chromium maintenance tooling, without network access."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'scripts'))
import sign_update
from check_cef_updates import available_updates


class CefMaintenanceTests(unittest.TestCase):
    def test_numeric_stable_comparison_ignores_beta_and_index_order(self):
        pinned = {f'cef-{p}': {'version': '1+chromium-154.0.8037.98'}
                  for p in ('linux64', 'windows64')}
        versions = [dict(channel=channel, chromium_version=version, cef_version='test')
                    for channel, version in [('stable', '99.0.0.0'), ('beta', '160.0.0.0'),
                                             ('stable', '154.0.8037.98')]]
        index = {p: {'versions': list(versions)} for p in ('linux64', 'windows64')}
        self.assertEqual(available_updates(index, pinned), [])
        index['linux64']['versions'].append(dict(channel='stable', chromium_version='154.0.8037.99', cef_version='new'))
        self.assertEqual(len(available_updates(index, pinned)), 1)


@unittest.skipUnless(shutil.which('openssl'), 'OpenSSL command line is required')
class SigningTests(unittest.TestCase):
    def test_signs_exact_bytes_and_rejects_untrusted_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'common').mkdir()
            key = root/'key.pem'
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(key)],
                           check=True, capture_output=True)
            public = subprocess.run(['openssl', 'pkey', '-in', str(key), '-pubout'],
                                    check=True, capture_output=True).stdout
            (root/'common/update_key.hpp').write_text(public.decode())
            artifact = root/'aura.run'
            artifact.write_bytes(b'abc')
            with patch.object(sign_update, 'ROOT', root):
                manifest, signature = sign_update.sign(key, '0.3.0', 5, {'linux-run': artifact}, root/'output')
                # PEM identity must survive native OpenSSL / Git LF and CRLF differences.
                (root/'common/update_key.hpp').write_bytes(public.replace(b'\r', b'').replace(b'\n', b'\r\n'))
                manifest, signature = sign_update.sign(key, '0.3.0', 5, {'linux-run': artifact}, root/'output')
                data = tomllib.loads(manifest.read_text())
                self.assertEqual(data['artifacts'][0]['size'], 3)
                self.assertEqual(data['artifacts'][0]['sha256'],
                                 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
                public_file = root/'public.pem'
                public_file.write_bytes(public)
                def verify():
                    return subprocess.run(['openssl', 'pkeyutl', '-verify', '-rawin', '-pubin',
                                           '-inkey', str(public_file), '-in', str(manifest),
                                           '-sigfile', str(signature)], capture_output=True).returncode
                self.assertEqual(verify(), 0)
                manifest.write_bytes(manifest.read_bytes()+b'\n')
                self.assertNotEqual(verify(), 0)
                with self.assertRaises(ValueError):
                    sign_update.sign(key, '../escape', 5, {'linux-run': artifact}, root/'bad')
                (root/'common/update_key.hpp').write_text('other public identity')
                with self.assertRaises(ValueError):
                    sign_update.sign(key, '0.3.0', 5, {'linux-run': artifact}, root/'bad')
