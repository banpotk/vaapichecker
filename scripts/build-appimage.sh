#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
source "$root/scripts/appimage-common.sh"
engine_request=auto
native=0
check_compat=1
run_tests=1
jobs=$(getconf _NPROCESSORS_ONLN)
(( jobs > 4 )) && jobs=4
work="$root/build-appimage"
output="$root/dist"

usage()
{
    cat <<'EOF'
Usage: scripts/build-appimage.sh [options]

Build one AppImage for Ubuntu 24.04 and Debian 13 using an Ubuntu 24.04 baseline.
The default build runs in a Podman/Docker container, then tests both distributions.

  --engine auto|podman|docker  Container engine (default: auto)
  --native                    Build on the host; requires Ubuntu 24.04 + dependencies
  --jobs N                    Compiler parallelism (default: up to 4 cores)
  --output-dir PATH           Artifact directory (default: dist/)
  --skip-tests                Skip C++/QtTest cases; package smoke tests still run
  --skip-compat-check         Skip the Ubuntu/Debian container smoke tests
  -h, --help                  Show this help

Build cache: build-appimage/; downloads use fixed releases and SHA256 verification.
If tests/test_checker.cpp is absent, QtTest is skipped automatically.
Architecture follows the build host: x86_64 or aarch64 (no cross-compilation).
EOF
}
while (( $# )); do
    case $1 in
        --engine|--jobs|--output-dir)
            (( $# >= 2 )) || { printf 'Missing value for %s\n' "$1" >&2; exit 2; }
            case $1 in
                --engine) engine_request=$2 ;;
                --jobs) jobs=$2 ;;
                --output-dir) output=$2 ;;
            esac
            shift 2 ;;
        --native) native=1; shift ;;
        --skip-tests) run_tests=0; shift ;;
        --skip-compat-check) check_compat=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { printf '%s\n' '--jobs must be a positive integer.' >&2; exit 2; }
[[ "$engine_request" =~ ^(auto|podman|docker)$ ]] || { printf 'Invalid container engine: %s\n' "$engine_request" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { printf 'This script requires Linux.\n' >&2; exit 1; }
if [[ "$run_tests" == 1 && ! -f "$root/tests/test_checker.cpp" ]]; then
    printf 'Warning: tests/test_checker.cpp is missing; skipping QtTest. Package smoke tests will still run.\n' >&2
    run_tests=0
fi
mkdir -p "$work" "$output"
output=$(realpath "$output")
if [[ "$native" == 0 || "$check_compat" == 1 ]]; then
    engine=$(appimage_engine "$engine_request")
fi
if [[ "$native" == 1 ]]; then
    bash "$root/packaging/appimage/build.sh" "$root" "$work" "$output" "$jobs" "$run_tests"
else
    "$engine" build --tag vaapi-checker-appimage-builder:ubuntu24.04 \
        --file "$root/packaging/appimage/Dockerfile" "$root/packaging/appimage"
    appimage_mounts "$root" "$run_tests"
    user_options=(--user "$(id -u):$(id -g)")
    [[ "$engine" == podman ]] && user_options+=(--userns=keep-id)
    "$engine" run --rm --security-opt label=disable \
        "${user_options[@]}" \
        "${APPIMAGE_SOURCE_MOUNTS[@]}" \
        --volume "$work:/work:rw" --volume "$output:/output:rw" \
        vaapi-checker-appimage-builder:ubuntu24.04 \
        /source /work /output "$jobs" "$run_tests"
fi
version=$(sed -n 's/^project(VAAPIChecker VERSION \([^ ]*\).*/\1/p' "$root/CMakeLists.txt")
arch=$(uname -m)
[[ "$arch" == arm64 ]] && arch=aarch64
package="$output/VAAPI_Checker-$version-$arch.AppImage"
[[ -f "$package" ]] || { printf 'Package output was not found: %s\n' "$package" >&2; exit 1; }
if [[ "$check_compat" == 1 ]]; then
    bash "$root/scripts/verify-appimage.sh" --engine "$engine" "$package"
fi
printf '\nAppImage ready: %s\nSHA256: %s.sha256\n' "$package" "$package"
