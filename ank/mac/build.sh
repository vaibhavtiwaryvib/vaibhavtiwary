#!/usr/bin/env bash
# Builds Ank.app (Apple silicon + Intel) and Ank.dmg.
# On a Mac: needs the Xcode command line tools (xcode-select --install). Run: ./build.sh
# On Linux: needs clang + lld, a macOS SDK (set MACOS_SDK=/path/MacOSX14.5.sdk), rcodesign and python3 + pycdlib.
set -euo pipefail
cd "$(dirname "$0")"
OUT=${OUT:-build}
APP="$OUT/Ank.app"
rm -rf "$OUT"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

if [[ "$(uname)" == "Darwin" ]]; then
  SDK=$(xcrun --show-sdk-path); LD=(); LIPO=lipo
else
  SDK=${MACOS_SDK:?set MACOS_SDK to a macOS SDK folder}; LD=(-fuse-ld=lld); LIPO=$(command -v llvm-lipo || echo /usr/lib/llvm-18/bin/llvm-lipo)
fi
for arch in arm64 x86_64; do
  clang -target "$arch-apple-macos11.0" -isysroot "$SDK" "${LD[@]}" -fobjc-arc -O2 -Wall \
    -framework Cocoa -framework WebKit main.m -o "$OUT/Ank-$arch"
done
"$LIPO" -create "$OUT/Ank-arm64" "$OUT/Ank-x86_64" -output "$APP/Contents/MacOS/Ank"
rm "$OUT/Ank-arm64" "$OUT/Ank-x86_64"

cp Info.plist "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
cp ../app/index.html "$APP/Contents/Resources/Ank.html"

# Ad-hoc signature (free; not notarized, so macOS asks once — see "How to install.txt")
if [[ "$(uname)" == "Darwin" ]]; then codesign --force --deep --sign - "$APP"; else rcodesign sign "$APP" >/dev/null; fi

# Disk image
STAGE="$OUT/dmg"; mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
cp "How to install.txt" "$STAGE/"
if [[ "$(uname)" == "Darwin" ]]; then
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -volname Ank -srcfolder "$STAGE" -ov -format UDZO "$OUT/Ank.dmg"
else
  python3 make_dmg.py "$STAGE" "$OUT/Ank.dmg"
fi
echo "Built $APP and $OUT/Ank.dmg"
