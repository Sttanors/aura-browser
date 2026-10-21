"""Download installer assets, sign their exact hashes, upload channel metadata."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from sign_update import ROOT, sign
from release_identity import read_identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--key', type=Path, required=True)
    args = parser.parse_args()
    identity = read_identity()
    version, sequence = identity.version, identity.sequence
    if args.tag != 'v'+version:
        raise ValueError('Release tag must match the compiled browser version and sequence')
    gh = os.environ.get('GH_BIN', 'gh')
    with tempfile.TemporaryDirectory(prefix='aura-sign-release-') as temporary:
        root = Path(temporary)
        subprocess.run([gh, 'release', 'download', args.tag, '--repo', 'Sttanors/aura-browser',
                        '--dir', str(root), '--pattern', 'aura2-*', '--pattern', 'aura-browser*'], check=True)
        selected = {}
        for file in root.iterdir():
            if file.name.endswith('.sha256'):
                continue
            kind = {'aura2-windows64-setup.exe': 'windows-setup',
                    'aura2-windows64.zip': 'windows-portable',
                    'aura2-linux64.run': 'linux-run',
                    'aura2-linux64.zip': 'linux-portable'}.get(file.name)
            if file.suffix == '.deb': kind = 'linux-deb'
            if file.suffix == '.rpm': kind = 'linux-rpm'
            if file.name.endswith('.pkg.tar.gz'): kind = 'linux-arch'
            if kind:
                if kind in selected:
                    raise ValueError('Multiple release artifacts for '+kind)
                selected[kind] = file
        if not {'windows-setup', 'linux-run'} <= selected.keys():
            raise ValueError('The release must contain both Windows and Linux installers')
        metadata = sign(args.key, version, sequence, selected, root/'signed')
        subprocess.run([gh, 'release', 'upload', args.tag, '--repo', 'Sttanors/aura-browser',
                        '--clobber', *map(str, metadata)], check=True)


if __name__ == '__main__':
    main()
