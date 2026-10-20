"""Build a synthetic previous sequence for the isolated Linux upgrade test."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
from dependencies import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, choices=(1, 2, 4), default=2)
    args = parser.parse_args()
    if sys.platform != 'linux':
        raise RuntimeError('This installed-upgrade fixture is for Linux')
    stage = ROOT/'artifacts/updates-journey/source'
    if stage.is_symlink():
        raise ValueError('Fixture source directory must not be a symlink')
    stage.mkdir(parents=True, exist_ok=True)
    for name in ('app', 'common', 'core', 'render', 'ui', 'platform', 'cmake', 'design', 'scripts', 'resources'):
        shutil.copytree(ROOT/name, stage/name, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('__pycache__'))
    for name in ('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json'):
        shutil.copy2(ROOT/name, stage/name)
    if not (stage/'third_party').exists():
        (stage/'third_party').symlink_to(ROOT/'third_party', target_is_directory=True)
    path = stage/'common/update_key.hpp'
    identity = path.read_text()
    current = int(re.search(r'browser_release_sequence = (\d+)', identity)[1])
    if current < 2:
        raise ValueError('The fixture needs a previous release sequence')
    identity = re.sub(r'browser_release_sequence = \d+', f'browser_release_sequence = {current-1}', identity)
    identity = re.sub(r'browser_version = "[^"]+"', 'browser_version = "0.0.0-preview.previous"', identity)
    path.write_text(identity)
    subprocess.run(['cmake', '--preset', 'linux-x64-release', '-DBUILD_TESTING=OFF'], cwd=stage, check=True)
    subprocess.run(['cmake', '--build', '--preset', 'linux-x64-release', '--target', 'aura_browser',
                    '-j'+str(args.jobs)], cwd=stage, check=True)
    print('Previous-sequence fixture built; repository source was not changed.')


if __name__ == '__main__':
    main()
