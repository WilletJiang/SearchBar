#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
TASK_BUILD_DIR=$(mktemp -d /tmp/searchbar-build.XXXXXX)
trap 'rm -rf "$TASK_BUILD_DIR"' EXIT
TASK_SDK=$(xcrun --show-sdk-path)
mkdir -p dist/SearchBar.app/Contents/MacOS dist/SearchBar.app/Contents/Resources
for source in Sources/SearchCore/*.c; do
  xcrun clang -O3 -DNDEBUG -Wall -Wextra -isysroot "$TASK_SDK" -mmacosx-version-min=26.0 -c "$source" -o "$TASK_BUILD_DIR/${source:t:r}.o"
done
TASK_SWIFT_SOURCES=(Sources/App/*.swift Sources/UI/*.swift Sources/Search/*.swift Sources/System/*.swift)
TASK_SWIFT_FLAGS=()
if [[ "${1:-}" == "--benchmark" ]]; then
  TASK_SWIFT_SOURCES+=(Tests/AppPerformance.swift Tests/BenchmarkMemory.swift)
  TASK_SWIFT_FLAGS+=(-DSEARCHBAR_BENCHMARK)
fi
xcrun swiftc -O -whole-module-optimization -swift-version 5 -sdk "$TASK_SDK" -target arm64-apple-macos26.0 \
  "${TASK_SWIFT_FLAGS[@]}" -import-objc-header Sources/SearchCore/SearchCore.h "${TASK_SWIFT_SOURCES[@]}" \
  "$TASK_BUILD_DIR"/*.o -lsqlite3 -framework AppKit -framework Carbon -framework Contacts \
  -framework CoreServices -framework CoreFoundation -framework PDFKit -framework Quartz -framework ServiceManagement \
  -o "$TASK_BUILD_DIR/SearchBar"
cp "$TASK_BUILD_DIR/SearchBar" dist/SearchBar.app/Contents/MacOS/SearchBar
cp Resources/Info.plist dist/SearchBar.app/Contents/Info.plist
cp LICENSE dist/SearchBar.app/Contents/Resources/LICENSE
if [[ -f Resources/AppIcon.icns ]]; then cp Resources/AppIcon.icns dist/SearchBar.app/Contents/Resources/AppIcon.icns; fi
codesign --force --sign - --identifier dev.searchbar.app dist/SearchBar.app
du -sh dist/SearchBar.app
