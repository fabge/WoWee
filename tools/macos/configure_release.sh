#!/bin/bash
# Configure and build a release tree for the local machine's architecture.
#
# The Homebrew prefixes are the whole content of this script: without the
# PKG_CONFIG_PATH entries, CMake finds the system copies of ffmpeg and OpenSSL
# and configures a build that links against the wrong libraries.
#
#   tools/macos/configure_release.sh [build-dir]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build-release-arm64}"

BREW="$(brew --prefix)"
export PKG_CONFIG_PATH="${BREW}/lib/pkgconfig:$(brew --prefix ffmpeg)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix vulkan-loader)/lib/pkgconfig:$(brew --prefix shaderc)/lib/pkgconfig"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    -DCMAKE_PREFIX_PATH="${BREW}" \
    -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"

cmake --build "${BUILD_DIR}" -j "$(sysctl -n hw.logicalcpu)"
