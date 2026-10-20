"""Wrap a verified portable ZIP in an offline, per-user shell installer."""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def build(archive: Path, output: Path):
    with tempfile.TemporaryDirectory(prefix='aura-installer-') as temporary:
        payload = Path(temporary)/'payload.tar.gz'
        with zipfile.ZipFile(archive) as source, tarfile.open(payload, 'w:gz') as target:
            assert 'Aura2/aura_browser' in source.namelist(), 'Not an Aura Linux runtime archive'
            for entry in source.infolist():
                path = PurePosixPath(entry.filename)
                if path.is_absolute() or '..' in path.parts or path.parts[0] != 'Aura2':
                    raise ValueError(f'Unsafe payload path: {path}')
                if entry.is_dir():
                    continue
                info = tarfile.TarInfo(entry.filename)
                info.size = entry.file_size
                info.mode = 0o755 if path.name in {'aura_browser', 'chrome-sandbox'} else 0o644
                with source.open(entry) as content:
                    target.addfile(info, content)
        digest = hashlib.sha256(payload.read_bytes()).hexdigest()
        header = (ROOT/'packaging/linux/install.sh').read_text().replace('@PAYLOAD_SHA256@', digest)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('wb') as result, payload.open('rb') as data:
            result.write(header.encode())
            import shutil
            shutil.copyfileobj(data, result)
    output.chmod(0o755)
    output.with_suffix(output.suffix+'.sha256').write_text(hashlib.sha256(output.read_bytes()).hexdigest()+'  '+output.name+'\n')
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', type=Path, default=ROOT/'artifacts/aura2-linux64.zip')
    parser.add_argument('--output', type=Path, default=ROOT/'artifacts/aura2-linux64.run')
    args = parser.parse_args()
    build(args.archive, args.output)
