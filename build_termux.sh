#!/data/data/com.termux/files/usr/bin/bash
# Termux e chalao: bash build_termux.sh
set -e
termux-setup-storage || true
pkg update -y
pkg install -y clang cmake ninja git glslang vulkan-headers
cd "$(dirname "$0")"
mkdir -p gen
glslangValidator -V vs.vert --vn vs_spv -o gen/vs_spv.h
glslangValidator -V fs.frag --vn fs_spv -o gen/fs_spv.h
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out
cp out/libfoxpostfx.so /sdcard/Download/libfoxpostfx.so
echo "Done: /sdcard/Download/libfoxpostfx.so"
