#!/usr/bin/env bash
set -euo pipefail

# Meant to be executed inside the kvasir-firmware-builder Docker container.
# If positional arguments are given (host-relative source paths, e.g. from a
# pre-commit hook), only those files are linted. Otherwise every source file
# in the compile DB is linted.

JOBS="${CPUS:-1}"

FW_SRC_REGEX='^/workspace/firmware/app/.*\.(cpp|c)$'
FW_HDR_REGEX='/workspace/firmware/app/'

# Translate host-relative paths (`firmware/app/foo.cpp`) to Docker-absolute
# (`/workspace/firmware/app/foo.cpp`). Filter to source extensions - headers
# get covered transitively via --header-filter.
ALLOWED_FILES=()
for f in "$@"; do
    case "$f" in
    *.cpp | *.c) ALLOWED_FILES+=("/workspace/$f") ;;
    esac
done

# clang-tidy uses libclang to parse. It does not know about arm-none-eabi-g++'s
# implicit system include paths, so <cstddef>, <array>, HAL headers, etc. would
# be missing. Query the paths at runtime and inject them.
ARM_EXTRA_ARGS=()
while IFS= read -r path; do
    ARM_EXTRA_ARGS+=("--extra-arg-before=-isystem${path}")
done < <(arm-none-eabi-g++ -E -x c++ - -v </dev/null 2>&1 \
         | sed -n '/^#include <\.\.\.> search starts here:/,/^End of search list\./p' \
         | sed '1d;$d' \
         | sed 's/^[[:space:]]*//')

(cd /workspace/firmware  && cmake --fresh --preset slot-a >/dev/null 2>&1)

# Serialize the allowlist for the python filter. Empty list means "no filter".
ALLOWLIST=""
if [[ ${#ALLOWED_FILES[@]} -gt 0 ]]; then
    ALLOWLIST=$(printf '%s\n' "${ALLOWED_FILES[@]}")
fi

run_tidy() {
    local build="$1"; local src_regex="$2"; local hdr_regex="$3"; shift 3

    mapfile -t SOURCES < <(python3 -c '
import json, re, sys
db = json.load(open(sys.argv[1]))
pat = re.compile(sys.argv[2])
allow_raw = sys.argv[3]
allow = set(allow_raw.splitlines()) if allow_raw else None
seen = set()
for e in db:
    f = e["file"]
    if not pat.match(f):
        continue
    if allow is not None and f not in allow:
        continue
    if f in seen:
        continue
    seen.add(f)
    print(f)
' "${build}/compile_commands.json" "${src_regex}" "${ALLOWLIST}")

    if [[ ${#SOURCES[@]} -eq 0 ]]; then
        return
    fi

    echo "==> clang-tidy: ${build} (${#SOURCES[@]} file(s))"
    printf '%s\0' "${SOURCES[@]}" | xargs -0 -n1 -P "${JOBS}" \
        clang-tidy -p "${build}" \
                   --header-filter="${hdr_regex}" \
                   --quiet \
                   "$@"
}

run_tidy /workspace/firmware/build/slot-a  "$FW_SRC_REGEX"   "$FW_HDR_REGEX"   "${ARM_EXTRA_ARGS[@]}"
