#!/bin/bash
# Install a built bundle as /Applications/Wowee.app.
#
# The installed app is a deployment artifact. It is replaced in place and no
# backup is kept: it holds no state, upstream publishes releases, and any
# earlier build can be rebuilt from this fork's history.
#
# What must survive an upgrade lives outside the bundle and is never touched
# here: the extracted game data in ~/Library/Application Support/Wowee/Data,
# and the configuration in ~/.wowee. This checks the data directory before and
# after and fails if it moved.
#
#   tools/macos/install_app.sh [source-app] [destination-app]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOURCE_APP="${1:-${REPO_ROOT}/dist/Wowee.app}"
DEST_APP="${2:-/Applications/Wowee.app}"
DATA_DIR="${HOME}/Library/Application Support/Wowee/Data"

if [ ! -d "${SOURCE_APP}" ]; then
    echo "no bundle at ${SOURCE_APP}; build one first:" >&2
    echo "  tools/macos/make_app.sh" >&2
    exit 1
fi

# Replacing a running app leaves the running copy on deleted inodes and the new
# one half-launched.
if pgrep -f '/Wowee.app/Contents/MacOS/wowee_bin' >/dev/null; then
    echo 'Wowee is running; quit it before installing' >&2
    exit 1
fi

DATA_BEFORE=""
if [ -d "${DATA_DIR}" ]; then
    DATA_BEFORE="$(stat -f '%i:%m:%z' "${DATA_DIR}")"
fi

echo "==> installing ${SOURCE_APP} as ${DEST_APP}"
rm -rf "${DEST_APP}"
# ditto rather than cp: it preserves the signature seal and extended
# attributes, and cp -R has been known to break the former.
ditto "${SOURCE_APP}" "${DEST_APP}"

echo "==> verifying"
bash "${REPO_ROOT}/tools/macos/verify_bundle.sh" "${DEST_APP}" "$(uname -m)" | tail -1
bash "${REPO_ROOT}/tools/macos/verify_signature.sh" "${DEST_APP}" - | tail -1

if [ -n "${DATA_BEFORE}" ]; then
    DATA_AFTER="$(stat -f '%i:%m:%z' "${DATA_DIR}")"
    if [ "${DATA_BEFORE}" != "${DATA_AFTER}" ]; then
        echo "game data changed during install: ${DATA_BEFORE} -> ${DATA_AFTER}" >&2
        exit 1
    fi
    echo "game data untouched (${DATA_AFTER})"
fi

# Any bundle a previous upgrade left behind.
for stale in "${DEST_APP}".backup-*; do
    [ -e "${stale}" ] || continue
    echo "==> removing stale backup ${stale}"
    rm -rf "${stale}"
done

echo "installed ${DEST_APP}"
