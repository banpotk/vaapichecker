#!/usr/bin/env bash
set -Eeuo pipefail
package=${1:?AppImage path required}

export DEBIAN_FRONTEND=noninteractive
apt-get update
# These are system / graphics libraries only; Qt and FFmpeg are not installed.
apt-get install -y --no-install-recommends \
    libva2 libva-drm2 libgl1 libegl1 libopengl0 libfontconfig1 \
    libx11-6 libxcb1 libxkbcommon0 libglib2.0-0t64 \
    libwayland-client0 libwayland-cursor0 libwayland-egl1 fonts-dejavu-core python3 \
    xvfb xauth weston
if dpkg-query -W -f='${db:Status-Abbrev}\n' 'libqt6core6*' 2>/dev/null | grep -q '^ii'; then
    printf 'Compatibility test must not rely on a system Qt6 installation.\n' >&2
    exit 1
fi
test_dir=$(mktemp -d)
weston_pid=
cleanup()
{
    if [[ -n "$weston_pid" ]]; then
        kill "$weston_pid" 2>/dev/null || true
        wait "$weston_pid" 2>/dev/null || true
    fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
cd "$test_dir"
"$package" --appimage-extract-and-run --version
"$package" --appimage-extract-and-run --demo --report "$test_dir/demo.json"
python3 - "$test_dir/demo.json" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
assert report['demo'] and report['available'] and report['capabilities']
PY
set +e
"$package" --appimage-extract-and-run --device /tmp/vaapi-checker-missing-gpu --report "$test_dir/missing.json"
status=$?
set -e
[[ "$status" == 2 ]] || { printf 'Expected unavailable-device exit code 2, got %s\n' "$status" >&2; exit 1; }
python3 - "$test_dir/missing.json" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
assert not report['available'] and report['error'] and not report['demo']
PY
QT_QPA_PLATFORM=offscreen "$package" --appimage-extract-and-run --demo --smoke-test
for theme in light dark; do
    QT_QPA_PLATFORM=offscreen "$package" --appimage-extract-and-run --demo --theme "$theme" --smoke-test
done
QT_QPA_PLATFORM=offscreen "$package" --appimage-extract-and-run --device /tmp/vaapi-checker-missing-gpu --smoke-test
xvfb-run -a env QT_QPA_PLATFORM=xcb "$package" --appimage-extract-and-run --demo --smoke-test
export XDG_RUNTIME_DIR="$test_dir/runtime"
mkdir -m 700 "$XDG_RUNTIME_DIR"
weston --backend=headless --renderer=pixman --socket=vaapi-checker-test --idle-time=0 \
    > "$test_dir/weston.log" 2>&1 &
weston_pid=$!
for (( attempt=0; attempt<100; ++attempt )); do
    [[ -S "$XDG_RUNTIME_DIR/vaapi-checker-test" ]] && break
    kill -0 "$weston_pid" 2>/dev/null || break
    sleep 0.1
done
[[ -S "$XDG_RUNTIME_DIR/vaapi-checker-test" ]] || { cat "$test_dir/weston.log" >&2; exit 1; }
WAYLAND_DISPLAY=vaapi-checker-test QT_QPA_PLATFORM=wayland QT_WAYLAND_SHELL_INTEGRATION=xdg-shell \
    "$package" --appimage-extract-and-run --demo --smoke-test 2>&1 | tee "$test_dir/wayland.log"
# Qt can return success even when the Wayland shell integration fails to load.
if grep -Eq 'No shell integration|Loading shell integration failed|Failed to load client buffer integration|Could not load' "$test_dir/wayland.log"; then
    printf 'Wayland GUI integration failed.\n' >&2
    exit 1
fi
source /etc/os-release
printf '\nCompatibility smoke test PASSED: %s %s\n' "$ID" "$VERSION_ID"
