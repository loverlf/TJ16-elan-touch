#!/bin/bash
set -e

cd "$(dirname "$0")"

# Working root for downloaded SDK + intermediate objects.
WORK="${WORK:-$PWD/build}"
mkdir -p "$WORK"
cd "$WORK"

# Download MacKernelSDK (IOKit kernel headers) if not present yet.
if [ ! -d MacKernelSDK ]; then
    echo "[1/4] downloading MacKernelSDK"
    curl -L -o mksdk.tar.gz "https://github.com/acidanthera/MacKernelSDK/archive/refs/heads/master.tar.gz"
    tar -xzf mksdk.tar.gz
    mv MacKernelSDK-master MacKernelSDK
fi

ROOT="$PWD/.."
SRC="$ROOT/VoodooELAN"
OBJ="$WORK/obj"
KEXT="$WORK/VoodooELAN.kext"
mkdir -p "$OBJ"
rm -rf "$KEXT"
mkdir -p "$KEXT/Contents/MacOS"

echo "[2/4] compiling VoodooELAN.cpp"
xcrun --sdk macosx clang++ \
    -arch x86_64 -mmacosx-version-min=10.15 \
    -std=c++17 -O2 -g \
    -fapple-kext -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin \
    -fno-asynchronous-unwind-tables -mno-red-zone -fstack-usage -nostdinc \
    -DKERNEL -DKERNEL_PRIVATE -D__APPLE__ -D__MACH__ \
    -I"$WORK/MacKernelSDK/Headers" \
    -I"$SRC" \
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-vla \
    -fno-common -fasm-blocks -mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse -ftree-vectorize \
    -fno-non-call-exceptions \
    -c "$SRC/VoodooELAN.cpp" -o "$OBJ/VoodooELAN.o"

echo "[3/4] linking VoodooELAN.kext"
xcrun --sdk macosx clang++ \
    -arch x86_64 -mmacosx-version-min=10.15 \
    -nostdlib -fapple-kext -Wl,-kext \
    -Wl,-sectcreate,__TEXT,__info_plist,"$SRC/Info.plist" \
    -fno-exceptions -fno-rtti \
    "$OBJ/VoodooELAN.o" -o "$KEXT/Contents/MacOS/VoodooELAN"

echo "[4/4] packaging kext"
cp "$SRC/Info.plist" "$KEXT/Contents/Info.plist"

# Ad-hoc sign so modern macOS accepts the load (OpenCore ignores signing,
# but ad-hoc keeps things well-formed).
codesign --force --sign - "$KEXT" 2>/dev/null || true

# Stage a clean distributable that contains ONLY the kext. Uploading the
# kext directory directly would make upload-artifact flatten it into a bare
# "Contents", losing the VoodooELAN.kext wrapper.
DIST="$ROOT/dist"
rm -rf "$DIST"
mkdir -p "$DIST"
cp -R "$KEXT" "$DIST/"

echo "Done: $KEXT"
echo "Dist: $DIST/VoodooELAN.kext"
