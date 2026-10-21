#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'Build on Linux x86_64.' >&2; exit 1; }
[[ -f "$task_root/third_party/src/cef-linux64/include/cef_version.h" ]] || {
    echo 'Run python3 scripts/bootstrap.py --fetch-only first.' >&2; exit 1;
}
task_rustup=$(rustup show home)
task_cargo_bin=$(dirname -- "$(command -v cargo)")
mkdir -p "$task_root/artifacts/native-build-root" "$task_root/artifacts/native-packages"
docker build -t aura-package-build:debian13 "$task_root/packaging/linux/native" -f "$task_root/packaging/linux/native/Dockerfile.debian"
task_mounts=(-v "$task_root:/source:ro" -v "$task_root/artifacts/native-build-root:/work" -v "$task_root/artifacts/native-packages:/output"
    --mount type=tmpfs,destination=/source/local
    --mount type=tmpfs,destination=/source/.aura-profile
    --mount type=tmpfs,destination=/source/.git)
docker run --rm --user "$(id -u):$(id -g)" --cpus=4 --memory=6g --memory-swap=6g --pids-limit=512 \
    -e HOME=/tmp/aura-build-home -e RUSTUP_HOME=/opt/rustup -e CARGO_HOME=/work/third_party/cargo-home \
    -e PATH=/opt/cargo-bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    "${task_mounts[@]}" -v "$task_rustup:/opt/rustup:ro" -v "$task_cargo_bin:/opt/cargo-bin:ro" \
    aura-package-build:debian13 sh /source/packaging/linux/native/build-debian.sh
docker run --rm --user "$(id -u):$(id -g)" --cpus=4 --memory=6g --memory-swap=6g \
    "${task_mounts[@]}" aura-package-build:debian13 \
    python3 /source/scripts/build_native_linux.py --archive /output/aura2-linux64-debian13.zip --output /output
if [[ ${1:-} == --test ]]; then
    for task_family in debian arch fedora; do
        case $task_family in
            debian) task_image=debian:trixie-slim; task_package=$(python3 "$task_root/scripts/release_identity.py" --package debian) ;;
            arch) task_image=archlinux:base; task_package=$(python3 "$task_root/scripts/release_identity.py" --package arch) ;;
            fedora) task_image=fedora:44; task_package=$(python3 "$task_root/scripts/release_identity.py" --package fedora) ;;
        esac
        task_version=$(python3 "$task_root/scripts/release_identity.py" --version)
        if [[ $task_family == arch ]]; then
            task_package_version="$(python3 "$task_root/scripts/release_identity.py" --arch-version)-1"
        else
            task_package_version="$(python3 "$task_root/scripts/release_identity.py" --native-version)-1"
        fi
        docker run --rm --cpus=2 --memory=2g --memory-swap=2g --pids-limit=512 \
            -e AURA_PACKAGE_TEST_CONTAINER=1 -v "$task_root:/source:ro" \
            --mount type=tmpfs,destination=/source/local \
            --mount type=tmpfs,destination=/source/.aura-profile \
            --mount type=tmpfs,destination=/source/.git \
            -v "$task_root/artifacts/native-packages:/packages:ro" "$task_image" \
            sh /source/packaging/linux/native/test-installed.sh "$task_family" "/packages/$task_package" "$task_version" "$task_package_version"
    done
fi
