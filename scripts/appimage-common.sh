#!/usr/bin/env bash

appimage_engine()
{
    local requested=${1:-auto} candidate
    for candidate in podman docker; do
        [[ "$requested" != auto && "$candidate" != "$requested" ]] && continue
        if command -v "$candidate" >/dev/null && "$candidate" info >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    printf 'No accessible container engine. Install/start Podman or Docker, or use --native on Ubuntu 24.04.\n' >&2
    return 1
}

appimage_mounts()
{
    # Only build inputs are shared with the container; no home directory is mounted.
    local root=$1 run_tests=${2:-1} part
    APPIMAGE_SOURCE_MOUNTS=()
    for part in CMakeLists.txt src packaging; do
        APPIMAGE_SOURCE_MOUNTS+=(--volume "$root/$part:/source/$part:ro")
    done
    if [[ "$run_tests" == 1 && -d "$root/tests" ]]; then
        APPIMAGE_SOURCE_MOUNTS+=(--volume "$root/tests:/source/tests:ro")
    fi
}
