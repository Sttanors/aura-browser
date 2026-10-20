"""Sign release metadata with the separately stored Ed25519 channel key."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
KINDS = {
    'windows-setup': ('windows-x64', 'setup'),
    'windows-portable': ('windows-x64', 'portable'),
    'linux-run': ('linux-x64', 'run'),
    'linux-portable': ('linux-x64', 'portable'),
    'linux-deb': ('linux-x64', 'deb'),
    'linux-rpm': ('linux-x64', 'rpm'),
    'linux-arch': ('linux-x64', 'arch'),
}


def sign(key: Path, version: str, sequence: int, artifacts: dict[str, Path], output: Path,
         expires: int | None = None):
    def token(value):
        return bool(value) and len(value) <= 160 and value[0] != '.' and all(
            c.isascii() and (c.isalnum() or c in '._-') for c in value)
    if not token(version) or sequence < 1 or not artifacts or not artifacts.keys() <= KINDS.keys():
        raise ValueError('Invalid version, sequence or artifact kinds')
    public = subprocess.run(['openssl', 'pkey', '-in', str(key), '-pubout'],
                            check=True, capture_output=True).stdout.decode('ascii')
    def identity(text):
        match = re.search(r'-----BEGIN PUBLIC KEY-----(.*?)-----END PUBLIC KEY-----', text, re.S)
        return ''.join(match[1].split()) if match else None
    if not identity(public) or identity(public) != identity((ROOT/'common/update_key.hpp').read_text()):
        raise ValueError('Signing key does not match the public key trusted by Aura')
    now = int(time.time())
    expires = expires if expires is not None else now + 90 * 86400
    if not now < expires <= now + 180 * 86400:
        raise ValueError('Manifest expiry must be within the next 180 days')
    lines = ['schema = 1', f'version = {json.dumps(version)}',
             f'tag = {json.dumps("v"+version)}', f'sequence = {sequence}', f'expires = {expires}']
    names = set()
    for kind, path in sorted(artifacts.items()):
        if not token(path.name) or path.name in names or path.is_symlink() or not path.is_file():
            raise ValueError('Invalid, duplicate or non-regular artifact')
        names.add(path.name)
        size = path.stat().st_size
        if not 0 < size <= 2 * 1024**3:
            raise ValueError('Artifact exceeds size budget or is empty')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        platform, artifact_kind = KINDS[kind]
        lines += ['', '[[artifacts]]', f'platform = {json.dumps(platform)}',
                  f'kind = {json.dumps(artifact_kind)}', f'filename = {json.dumps(path.name)}',
                  f'sha256 = "{digest}"', f'size = {size}']
    output.mkdir(parents=True, exist_ok=True)
    manifest = output/'aura-update.toml'
    manifest.write_text('\n'.join(lines)+'\n', encoding='utf-8', newline='\n')
    signature = output/'aura-update.toml.sig'
    subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', str(key),
                    '-in', str(manifest), '-out', str(signature)], check=True)
    if signature.stat().st_size != 64:
        signature.unlink(missing_ok=True)
        raise ValueError('Expected an Ed25519 signature')
    return manifest, signature


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--key', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--sequence', type=int, required=True)
    parser.add_argument('--artifact', action='append', required=True, help='KIND=PATH')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pairs = [item.split('=', 1) for item in args.artifact]
    if any(len(pair) != 2 for pair in pairs) or len({p[0] for p in pairs}) != len(pairs):
        parser.error('Each artifact must use a unique KIND=PATH')
    for result in sign(args.key, args.version, args.sequence,
                       {kind: Path(path) for kind, path in pairs}, args.output):
        print(result)
