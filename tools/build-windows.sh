#!/bin/sh
# Build a Windows version of Dum on Linux (needs: sudo apt install mingw-w64 cmake curl).
# Result: build-win/Dum-windows/ (dum.exe + DLLs + data) and build-win/Dum-windows.zip
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=$ROOT/build-win
DEPS=$OUT/deps
SDL_VER=2.30.9
IMG_VER=2.8.2
mkdir -p "$DEPS"
cd "$DEPS"
if [ ! -d "SDL2-$SDL_VER" ]; then
    echo "Downloading SDL2 $SDL_VER (MinGW development files)..."
    curl -L -o sdl2.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
    tar xzf sdl2.tar.gz
fi
if [ ! -d "SDL2_image-$IMG_VER" ]; then
    echo "Downloading SDL2_image $IMG_VER (MinGW development files)..."
    curl -L -o sdl2_image.tar.gz "https://github.com/libsdl-org/SDL_image/releases/download/release-$IMG_VER/SDL2_image-devel-$IMG_VER-mingw.tar.gz"
    tar xzf sdl2_image.tar.gz
fi
SDL=$DEPS/SDL2-$SDL_VER/x86_64-w64-mingw32
IMG=$DEPS/SDL2_image-$IMG_VER/x86_64-w64-mingw32

cmake -S "$ROOT" -B "$OUT/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake" \
    -DDUM_WIN_DEPS="$SDL;$IMG" \
    -DCMAKE_PREFIX_PATH="$SDL;$IMG"
cmake --build "$OUT/build" -j"$(nproc)"

PKG=$OUT/Dum-windows
rm -rf "$PKG"
mkdir -p "$PKG"
cp "$OUT/build/dum.exe" "$OUT/build/dum_tool.exe" "$PKG/"
cp "$SDL/bin/SDL2.dll" "$IMG/bin/SDL2_image.dll" "$PKG/"
cp -r "$ROOT/data" "$PKG/data"
cp "$ROOT/README.md" "$PKG/README.txt"
cp "$ROOT/LICENSE" "$PKG/LICENSE.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$PKG/THIRD_PARTY_NOTICES.txt"
cp -r "$ROOT/docs" "$PKG/docs"
cd "$OUT"
rm -f Dum-windows.zip
if command -v zip >/dev/null; then zip -qr Dum-windows.zip Dum-windows; echo "Packaged $OUT/Dum-windows.zip"; fi
echo "Windows build ready in $PKG"
