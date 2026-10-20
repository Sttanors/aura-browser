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
task_mounts=(-v "$task_root:/source:ro" -v "$task_root/artifacts/native-build-root:/work" -v "$task_root/artifacts/native-packages:/output")
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
            debian) task_image=debian:trixie-slim; task_package='aura-browser_0.2.0-preview.3-1_amd64.deb' ;;
            arch) task_image=archlinux:base; task_package=aura-browser-0.2.0rc3-1-x86_64.pkg.tar.gz ;;
            fedora) task_image=fedora:44; task_package='aura-browser-0.2.0-preview.3-1.x86_64.rpm' ;;
        esac
        docker run --rm --cpus=2 --memory=2g --memory-swap=2g --pids-limit=512 \
            -e AURA_PACKAGE_TEST_CONTAINER=1 -v "$task_root:/source:ro" \
            -v "$task_root/artifacts/native-packages:/packages:ro" "$task_image" \
            sh /source/packaging/linux/native/test-installed.sh "$task_family" "/packages/$task_package"
    done
fi
