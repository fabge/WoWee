#!/bin/bash
# Point the repository at this machine's extracted game data.
#
# 54 of the sweeps in tools/ read the real interface through Data/interface,
# and skip themselves when it is not there. Nothing created it, so 57 of the 92
# sweeps in sweep_guard skipped on every run, locally and in CI - the FrameXML
# half of the safety net AGENTS.md describes was silently absent while the
# suite reported "every sweep at or under its ceiling".
#
# The link points outside the repository at proprietary extracted data.
# Data/* is gitignored, so it cannot be committed.
#
#   tools/link_local_data.sh [data-root]
#
# WOWEE_DATA_ROOT overrides the default location.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_ROOT="${1:-${WOWEE_DATA_ROOT:-${HOME}/Library/Application Support/Wowee/Data}}"
INTERFACE="${DATA_ROOT}/expansions/wotlk/interface"

if [ ! -d "${INTERFACE}" ]; then
    echo "no extracted interface at ${INTERFACE}" >&2
    echo "extract the game data first, or pass the data root as an argument" >&2
    exit 1
fi

ln -sfn "${INTERFACE}" "${REPO_ROOT}/Data/interface"
echo "Data/interface -> ${INTERFACE}"

# The helper binaries, where the sweeps that drive them look for them. The
# tools hardcode build/bin/<tool>; this checkout builds into build-review per
# AGENTS.md, so each reported itself missing and the guard read that as a
# matcher gone blind - a clean report it had not earned. framexml_run needs
# -DWOWEE_BUILD_FRAMEXML_RUN=ON; the others are built by default.
for tool in framexml_run framexml_emit blp_convert; do
    for build in build-review build-release-arm64 build-clang; do
        binary="${REPO_ROOT}/${build}/bin/${tool}"
        [ -x "${binary}" ] || continue
        mkdir -p "${REPO_ROOT}/build/bin"
        ln -sfn "${binary}" "${REPO_ROOT}/build/bin/${tool}"
        echo "build/bin/${tool} -> ${build}/bin/${tool}"
        break
    done
done

# The DBC tables a few sweeps compare their layouts against. The extractor
# writes them where the game does, under dbfilesclient.
DB="${DATA_ROOT}/expansions/wotlk/dbfilesclient"
if [ -d "${DB}" ]; then
    ln -sfn "${DB}" "${REPO_ROOT}/Data/db"
    echo "Data/db -> ${DB}"
fi
