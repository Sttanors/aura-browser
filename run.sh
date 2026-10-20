#!/usr/bin/env bash
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_binary="$task_root/build/linux-x64-release/aura_browser"
if [[ ! -x "$task_binary" ]]; then
    echo 'Aura ещё не собрана. Из каталога проекта выполните:' >&2
    echo 'python3 scripts/bootstrap.py --jobs 8' >&2
    echo 'cmake --preset linux-x64-release' >&2
    echo 'cmake --build --preset linux-x64-release --parallel 8' >&2
    exit 1
fi
exec "$task_binary" "--data-dir=$task_root/.aura-profile" "$@"
