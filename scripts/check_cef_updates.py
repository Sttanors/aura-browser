"""Check the official CEF index without changing the pinned runtime."""
from __future__ import annotations
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
INDEX = 'https://cef-builds.spotifycdn.com/index.json'


def available_updates(index: dict, artifacts: dict) -> list[str]:
    updates = []
    for platform in ('linux64', 'windows64'):
        locked = artifacts['cef-'+platform]['version'].split('chromium-', 1)[1]
        number = lambda text: tuple(int(part) for part in text.split('.'))
        stable = [item for item in index[platform]['versions'] if item.get('channel') == 'stable']
        if not stable:
            raise ValueError('No stable CEF builds in official index for '+platform)
        newest = max(stable, key=lambda item: number(item['chromium_version']))
        if number(newest['chromium_version']) > number(locked):
            updates.append(f'{platform}: Chromium {locked} -> {newest["chromium_version"]} '
                           f'(CEF {newest["cef_version"]})')
    return updates


if __name__ == '__main__':
    with urllib.request.urlopen(INDEX, timeout=30) as response:
        index = json.load(response)
    artifacts = json.loads((ROOT/'third_party/dependencies.lock.json').read_text())['artifacts']
    updates = available_updates(index, artifacts)
    if updates:
        print('\n'.join(updates))
        raise SystemExit('Update the dependency lock and validate both browser builds before release.')
    print('No newer stable Chromium runtime was found for the pinned platforms.')
