#!/bin/sh
set -eu
mkdir -p /work/third_party /work/resources
for directory in app common core render ui platform cmake design scripts tests packaging; do
    cp -a "/source/$directory" /work/
done
cp /source/CMakeLists.txt /source/CMakePresets.json /source/vcpkg.json /work/
[ -L /work/third_party/src ] || ln -s /source/third_party/src /work/third_party/src
[ -L /work/third_party/tools ] || ln -s /source/third_party/tools /work/third_party/tools
mkdir -p /work/third_party/cargo-home
cp -a /source/third_party/cargo-home/. /work/third_party/cargo-home/
cp -a /source/resources/. /work/resources/
cd /work
python3 scripts/build_skia.py --jobs 4
cmake --preset linux-x64-release -DAURA_BUILD_COMPATIBILITY_HARNESS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN'
cmake --build --preset linux-x64-release --parallel 4
ctest --preset linux-x64-release --output-on-failure
python3 scripts/package.py --output /output/aura2-linux64-debian13.zip
