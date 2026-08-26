#!/bin/bash
# Build Wowee.app from this checkout.
#
# The release workflow builds the bundle on a runner and signs it with a
# Developer ID identity. Locally there is no identity and no runner, and the
# same twelve steps - icns, binaries, assets, the shipped Data tree, MoltenVK
# and its ICD manifest, the dependency walk, Info.plist, signing, verification -
# were being retyped from memory each time. Doing that by hand is how a bundle
# ends up missing the ICD rewrite and failing to find a Vulkan driver at all.
#
#   tools/macos/make_app.sh [build-dir] [output-app] [identity]
#
# The default identity is "-", an ad-hoc signature: fine to run here, and not
# notarized. Do not describe a bundle built this way as notarized.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build-release-arm64}"
APP_PATH="${2:-${REPO_ROOT}/dist/Wowee.app}"
IDENTITY="${3:--}"
ARCH="$(uname -m)"

cd "${REPO_ROOT}"

for binary in wowee asset_extract; do
    if [ ! -x "${BUILD_DIR}/bin/${binary}" ]; then
        echo "missing ${BUILD_DIR}/bin/${binary}; configure and build first:" >&2
        echo "  tools/macos/configure_release.sh ${BUILD_DIR}" >&2
        exit 1
    fi
done

echo "==> staging ${APP_PATH}"
rm -rf "${APP_PATH}"
mkdir -p "${APP_PATH}/Contents/MacOS" \
         "${APP_PATH}/Contents/Frameworks" \
         "${APP_PATH}/Contents/Resources"

bash tools/macos/create_icns.sh assets/Wowee.png \
    "${APP_PATH}/Contents/Resources/Wowee.icns" >/dev/null

# The bundle's executable is wowee_bin: Info.plist names it, and the launcher
# resolves assets relative to Contents/Resources.
cp "${BUILD_DIR}/bin/wowee" "${APP_PATH}/Contents/MacOS/wowee_bin"
cp "${BUILD_DIR}/bin/asset_extract" "${APP_PATH}/Contents/MacOS/"
cp extract_assets.sh tools/asset_pipeline_gui.py "${APP_PATH}/Contents/Resources/"

# Music is the one asset group left out: it is large, optional, and the client
# resolves it from the user's data directory when present.
rsync -a --exclude='Original Music' \
    "${BUILD_DIR}/bin/assets/" "${APP_PATH}/Contents/Resources/assets/"

# The client's own addons, which AddonManager looks for as "addons" relative to
# the working directory - and the app sets that to Contents/Resources.
#
# Never copied here before, so WoweeAllBags has shipped in the repository and in
# no installed build since it was written: /allbags answered nothing, and the
# only way to see it was to run the binary out of the build tree. The dev build
# gets it from the copy_bundled_addons CMake target, which copies next to the
# executable and so covered the gap locally.
rsync -a "${REPO_ROOT}/addons/" "${APP_PATH}/Contents/Resources/addons/"

# The shipped Data tree - profiles, opcode tables, update fields, DBC layouts.
# Tracked files only: the extracted game data lives outside the bundle and must
# never be copied into it.
git ls-files Data/ | while read -r file; do
    mkdir -p "${APP_PATH}/Contents/Resources/$(dirname "${file}")"
    cp "${file}" "${APP_PATH}/Contents/Resources/${file}"
done

echo "==> vulkan"
MOLTENVK_PREFIX="$(brew --prefix molten-vk)"
cp -L "${MOLTENVK_PREFIX}/lib/libMoltenVK.dylib" \
    "${APP_PATH}/Contents/Frameworks/libMoltenVK.dylib"
cp -L "$(brew --prefix sdl3)/lib/libSDL3.dylib" \
    "${APP_PATH}/Contents/Frameworks/libSDL3.dylib"

# The ICD manifest ships an absolute Homebrew path. Rewritten to point inside
# the bundle, or the loader finds no driver on a machine without Homebrew.
mkdir -p "${APP_PATH}/Contents/Resources/vulkan/icd.d"
python3 - "${MOLTENVK_PREFIX}/etc/vulkan/icd.d/MoltenVK_icd.json" \
          "${APP_PATH}/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" <<'PY'
import json, pathlib, sys
source, destination = map(pathlib.Path, sys.argv[1:])
manifest = json.loads(source.read_text(encoding='utf-8'))
manifest['ICD']['library_path'] = '../../../Frameworks/libMoltenVK.dylib'
manifest['ICD']['is_portability_driver'] = True
destination.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
PY

echo "==> bundling dependencies"
python3 tools/macos/bundle_dependencies.py \
    --app "${APP_PATH}" \
    --search-dir "$(brew --prefix sdl2)/lib" \
    --search-dir "$(brew --prefix sdl3)/lib" \
    --search-dir "$(brew --prefix vulkan-loader)/lib" \
    --search-dir "$(brew --prefix openssl@3)/lib" \
    --search-dir "$(brew --prefix ffmpeg)/lib" \
    --search-dir "$(brew --prefix unicorn)/lib" \
    --search-dir "$(brew --prefix stormlib)/lib" \
    --search-dir "$(brew --prefix glm)/lib" \
    "${APP_PATH}/Contents/MacOS/wowee_bin" \
    "${APP_PATH}/Contents/MacOS/asset_extract" \
    "${APP_PATH}/Contents/Frameworks/libMoltenVK.dylib" \
    "${APP_PATH}/Contents/Frameworks/libSDL3.dylib" | tail -1

cat > "${APP_PATH}/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>wowee_bin</string>
  <key>CFBundleIdentifier</key><string>com.wowee.app</string>
  <key>CFBundleName</key><string>Wowee</string>
  <key>CFBundleIconFile</key><string>Wowee</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
</dict></plist>
PLIST

echo "==> signing (${IDENTITY})"
bash tools/macos/sign_app.sh "${APP_PATH}" "${IDENTITY}" >/dev/null 2>&1

echo "==> verifying"
# codesign reports on stderr, and its per-component chatter buries the verdict.
bash tools/macos/verify_signature.sh "${APP_PATH}" "${IDENTITY}" 2>&1 | tail -1
bash tools/macos/verify_bundle.sh "${APP_PATH}" "${ARCH}" 2>&1 | tail -1

echo "built ${APP_PATH} (${ARCH}, identity ${IDENTITY})"
