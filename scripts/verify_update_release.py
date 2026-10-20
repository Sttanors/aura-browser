"""Verify published update metadata and installer hashes, using only the public key."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import tomllib
from sign_update import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'v[A-Za-z0-9_.-]{1,160}', args.tag):
        parser.error('Invalid release tag')
    public = re.search(r'-----BEGIN PUBLIC KEY-----\n.*?-----END PUBLIC KEY-----\n',
                       (ROOT/'common/update_key.hpp').read_text(), re.S)[0]
    gh = os.environ.get('GH_BIN', 'gh')
    with tempfile.TemporaryDirectory(prefix='aura-verify-release-') as temporary:
        root = Path(temporary)
        def download(pattern):
            subprocess.run([gh, 'release', 'download', args.tag, '--repo', 'Sttanors/aura-browser',
                            '--dir', str(root), '--pattern', pattern], check=True)
        download('aura-update.toml')
        download('aura-update.toml.sig')
        metadata = root/'aura-update.toml'
        signature = root/'aura-update.toml.sig'
        if metadata.stat().st_size > 65536 or signature.stat().st_size != 64:
            raise ValueError('Invalid metadata/signature size')
        key = root/'public.pem'
        key.write_text(public)
        subprocess.run(['openssl', 'pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', str(key),
                        '-in', str(metadata), '-sigfile', str(signature)], check=True)
        data = tomllib.loads(metadata.read_text())
        if data['schema'] != 1 or data['tag'] != args.tag or data['tag'] != 'v'+data['version'] or data['sequence'] < 1:
            raise ValueError('Invalid signed release identity')
        if not time.time() < data['expires'] <= time.time()+180*86400:
            raise ValueError('Invalid signed metadata expiry')
        for artifact in data['artifacts']:
            filename = artifact['filename']
            if not re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]{0,159}', filename):
                raise ValueError('Invalid signed artifact name')
            download(filename)
            path = root/filename
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            if path.stat().st_size != artifact['size'] or digest != artifact['sha256']:
                raise ValueError('Published asset does not match its signed metadata: '+filename)
            print('Verified:', filename)
            path.unlink()


if __name__ == '__main__':
    main()
