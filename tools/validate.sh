#!/bin/bash
# Everything a change to this fork has to pass before it is committed.
#
# The suite is five things, and the last two need paths and scaffolding that
# are easy to get wrong: the FrameXML checks read extracted Blizzard data that
# cannot live in the repository, and the frame-emitted check wants a build/bin
# and a Data/interface that a checkout does not have. Both were run from memory
# and both were skipped whenever the incantation was not to hand.
#
#   tools/validate.sh [build-dir]
#
# WOWEE_DATA_ROOT overrides where the extracted interface is looked for. When
# there is none, the FrameXML arms report as skipped rather than failing: they
# need proprietary local data, and a public checkout has none.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build-review}"
JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)"
DATA_ROOT="${WOWEE_DATA_ROOT:-${HOME}/Library/Application Support/Wowee/Data}"
INTERFACE="${DATA_ROOT}/expansions/wotlk/interface"

cd "${REPO_ROOT}"

failures=()
skipped=()

step() { printf '\n==> %s\n' "$1"; }
note_failure() { failures+=("$1"); printf 'FAILED: %s\n' "$1"; }

step "build (${BUILD_DIR})"
if ! cmake --build "${BUILD_DIR}" -j "${JOBS}"; then
    note_failure "build"
    # Everything below runs what was just built.
    printf '\nbuild failed; nothing else ran\n'
    exit 1
fi

step "ctest"
if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure -j "${JOBS}"; then
    note_failure "ctest"
fi

step "whitespace"
if ! git diff --check; then
    note_failure "git diff --check"
fi

if [ -d "${INTERFACE}" ]; then
    for tree in framexml addons; do
        step "framexml_compile_check (${tree})"
        if ! "${BUILD_DIR}/bin/framexml_compile_check" "${INTERFACE}/${tree}" | tail -1; then
            note_failure "framexml_compile_check ${tree}"
        fi
    done

    step "framexml_frame_emitted_check"
    # The check resolves the emitter and the interface through paths a checkout
    # does not have. Links, then removed however this ends.
    mkdir -p build/bin
    ln -sf "${BUILD_DIR}/bin/framexml_emit" build/bin/framexml_emit
    ln -sfn "${INTERFACE}" Data/interface
    if ! python3 tools/framexml_frame_emitted_check.py | tail -3; then
        note_failure "framexml_frame_emitted_check"
    fi
    rm -f build/bin/framexml_emit Data/interface
    rmdir build/bin build 2>/dev/null || true
else
    skipped+=("FrameXML checks (no extracted data at ${INTERFACE})")
fi

printf '\n────────────────────────────────────────\n'
for entry in "${skipped[@]+"${skipped[@]}"}"; do
    printf 'skipped: %s\n' "${entry}"
done
if [ "${#failures[@]}" -eq 0 ]; then
    printf 'all checks passed\n'
    exit 0
fi
for entry in "${failures[@]}"; do
    printf 'failed:  %s\n' "${entry}"
done
exit 1
