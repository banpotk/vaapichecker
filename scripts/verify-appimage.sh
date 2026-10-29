#!/usr/bin/env bash
set -Eeuo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
source "$root/scripts/appimage-common.sh"
engine_request=auto
if [[ ${1:-} == --engine ]]; then
    (( $# >= 3 )) || { printf 'Usage: verify-appimage.sh [--engine auto|podman|docker] FILE.AppImage\n' >&2; exit 2; }
    engine_request=$2; shift 2
fi
if [[ $# != 1 || ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: scripts/verify-appimage.sh [--engine auto|podman|docker] FILE.AppImage\n'
    [[ ${1:-} == --help || ${1:-} == -h ]] && exit 0
    exit 2
fi
[[ -f "$1" ]] || { printf 'AppImage does not exist: %s\n' "$1" >&2; exit 2; }
[[ "$engine_request" =~ ^(auto|podman|docker)$ ]] || { printf 'Invalid container engine: %s\n' "$engine_request" >&2; exit 2; }
package=$(realpath "$1")
engine=$(appimage_engine "$engine_request")
for image in ubuntu:24.04 debian:13-slim; do
    printf '\nVerifying the same AppImage on %s (without system Qt6)\n' "$image"
    log="${package%.AppImage}.${image//[:\/]/-}.smoke-test.log"
    "$engine" run --rm --security-opt label=disable \
        --volume "$package:/package/VAAPI_Checker.AppImage:ro" \
        --volume "$root/packaging/appimage/runtime-test.sh:/test.sh:ro" \
        "$image" bash /test.sh /package/VAAPI_Checker.AppImage 2>&1 | tee "$log"
done
