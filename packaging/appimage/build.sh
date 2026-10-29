#!/usr/bin/env bash
set -Eeuo pipefail
trap 'printf "AppImage build failed at line %s\n" "$LINENO" >&2' ERR

source_dir=${1:?source directory required}
work_dir=${2:?work directory required}
output_dir=${3:?output directory required}
jobs=${4:-2}
run_tests=${5:-1}
source_dir=$(realpath "$source_dir")
mkdir -p "$work_dir" "$output_dir"
work_dir=$(realpath "$work_dir")
output_dir=$(realpath "$output_dir")
if [[ "$run_tests" == 1 && ! -f "$source_dir/tests/test_checker.cpp" ]]; then
    printf 'Warning: tests/test_checker.cpp is missing; skipping QtTest. Package smoke tests will still run.\n' >&2
    run_tests=0
fi

# A Debian 13 / newer host build can require a newer glibc than Ubuntu 24.04.
source /etc/os-release
if [[ "$ID" != ubuntu || "$VERSION_ID" != 24.04 ]]; then
    printf 'The shared Debian 13 / Ubuntu 24.04 artifact must be built on Ubuntu 24.04. Use scripts/build-appimage.sh (container mode).\n' >&2
    exit 1
fi
case $(uname -m) in
    x86_64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) printf 'Unsupported architecture: %s\n' "$(uname -m)" >&2; exit 1 ;;
esac
for command in cmake ninja pkg-config curl sha256sum patchelf file desktop-file-validate python3; do
    command -v "$command" >/dev/null || { printf 'Missing build dependency: %s\n' "$command" >&2; exit 1; }
done
pkg-config --atleast-version=6.4 Qt6Widgets
pkg-config --atleast-version=1.12 libva

tools_dir="$work_dir/tools/$arch"
mkdir -p "$tools_dir"
while read -r name version tool_arch digest url; do
    [[ "$name" == \#* || -z "$name" || "$tool_arch" != "$arch" ]] && continue
    destination="$tools_dir/${url##*/}"
    if [[ ! -f "$destination" ]]; then
        printf 'Downloading %s %s (%s)\n' "$name" "$version" "$arch"
        curl --fail --location --retry 3 --connect-timeout 20 --max-time 300 \
            --output "$destination.part" "$url"
        printf '%s  %s\n' "$digest" "$destination.part" | sha256sum --check --status
        mv "$destination.part" "$destination"
    fi
    printf '%s  %s\n' "$digest" "$destination" | sha256sum --check
    chmod +x "$destination"
done < "$source_dir/packaging/appimage/tools.lock"

build_dir="$work_dir/cmake"
appdir="$work_dir/AppDir"
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_CXX_COMPILER=/usr/bin/g++ -DBUILD_TESTING="$([[ "$run_tests" == 1 ]] && printf ON || printf OFF)"
cmake --build "$build_dir" --parallel "$jobs"
if [[ "$run_tests" == 1 ]]; then
    ctest --test-dir "$build_dir" --output-on-failure
fi
# Only this staging directory is recreated; the compiler and download caches remain.
[[ "$appdir" == "$work_dir/AppDir" && "$work_dir" != / ]] || exit 1
rm -rf -- "$appdir"
DESTDIR="$appdir" cmake --install "$build_dir"

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=/usr/bin/qmake6
export EXTRA_QT_MODULES='svg;waylandclient'
export EXTRA_PLATFORM_PLUGINS='libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so'
# Keep the host GPU stack together, including the C++ runtime used by GPU drivers.
export LINUXDEPLOY_EXCLUDED_LIBRARIES='libva*.so*;libdrm*.so*;libGL*.so*;libEGL*.so*;libOpenGL*.so*;libgbm.so*;libvulkan.so*;libwayland*.so*;libstdc++.so*;libgcc_s.so*'
IFS=';' read -r -a excluded_libraries <<< "$LINUXDEPLOY_EXCLUDED_LIBRARIES"
exclude_options=()
for library in "${excluded_libraries[@]}"; do
    exclude_options+=(--exclude-library "$library")
done
export PATH="$tools_dir:$PATH"
# The Qt deployment plugin does not include Qt6 Wayland shell/buffer integrations.
qt_plugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)
wayland_dependencies=()
for category in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    [[ -d "$qt_plugins/$category" ]] || { printf 'Missing Qt plugin directory: %s\n' "$category" >&2; exit 1; }
    mkdir -p "$appdir/usr/plugins/$category"
    cp -a "$qt_plugins/$category/." "$appdir/usr/plugins/$category/"
    wayland_dependencies+=(--deploy-deps-only "$appdir/usr/plugins/$category")
done
mkdir -p "$appdir/usr/share/doc/qt6-wayland"
install -m 644 /usr/share/doc/qt6-wayland/copyright "$appdir/usr/share/doc/qt6-wayland/copyright"
# linuxdeploy's default exclusion list assumes these GUI libraries are installed.
# Force deployment so a clean target needs only its system / GPU runtime.
extra_libraries=()
for library in libharfbuzz.so.0 libgpg-error.so.0 libX11-xcb.so.1 libSM.so.6 libICE.so.6 libuuid.so.1; do
    library_path=$(ldconfig -p | awk -v name="$library" '$1 == name { path=$NF } END { print path }')
    [[ -f "$library_path" ]] || { printf 'Missing GUI dependency: %s\n' "$library" >&2; exit 1; }
    extra_libraries+=(--library "$library_path")
done
"$tools_dir/linuxdeploy-$arch.AppImage" --appdir "$appdir" \
    "${exclude_options[@]}" \
    "${extra_libraries[@]}" \
    "${wayland_dependencies[@]}" \
    --executable "$appdir/usr/bin/vaapi-checker" \
    --desktop-file "$source_dir/packaging/io.github.vaapichecker.desktop" \
    --icon-file "$source_dir/packaging/vaapi-checker.svg" --plugin qt

# This Qt plugin release has its own exclusion list and can redeploy system libs.
# Enforce the host graphics boundary after every deployment pass.
for library in "${excluded_libraries[@]}"; do
    find "$appdir/usr/lib" -maxdepth 1 \( -type f -o -type l \) -name "$library" -print -delete
done

# AppRun uses relative RPATHs without leaking bundled libraries into host FFmpeg.
rm -f -- "$appdir/AppRun"
install -m 755 "$source_dir/packaging/appimage/AppRun" "$appdir/AppRun"
cat > "$appdir/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix=..
Plugins=plugins
Translations=translations
EOF
desktop-file-validate "$appdir/io.github.vaapichecker.desktop"
for plugin in libqxcb.so libqoffscreen.so libqwayland-egl.so libqwayland-generic.so; do
    [[ -f "$appdir/usr/plugins/platforms/$plugin" ]] || { printf 'Missing Qt platform plugin: %s\n' "$plugin" >&2; exit 1; }
done
for plugin in wayland-shell-integration/libxdg-shell.so wayland-graphics-integration-client/libqt-plugin-wayland-egl.so; do
    [[ -f "$appdir/usr/plugins/$plugin" ]] || { printf 'Missing Wayland integration: %s\n' "$plugin" >&2; exit 1; }
done
if find "$appdir/usr/lib" \( -name 'libva*.so*' -o -name '*_drv_video.so' -o -name 'libdrm*.so*' -o -name 'libwayland*.so*' -o -name 'libstdc++.so*' \) -print -quit | grep -q .; then
    printf 'Host GPU libraries were unexpectedly bundled.\n' >&2
    exit 1
fi
if ldd "$appdir/usr/bin/vaapi-checker" | grep -q 'not found'; then
    printf 'An AppDir executable dependency is missing.\n' >&2
    exit 1
fi
QT_QPA_PLATFORM=offscreen "$appdir/AppRun" --demo --smoke-test

version=$("$appdir/AppRun" --version | awk '{print $NF}')
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { printf 'Invalid application version: %s\n' "$version" >&2; exit 1; }
package="VAAPI_Checker-$version-$arch.AppImage"
export ARCH="$arch"
"$tools_dir/appimagetool-$arch.AppImage" --runtime-file "$tools_dir/runtime-$arch" \
    "$appdir" "$output_dir/$package"
chmod +x "$output_dir/$package"
"$output_dir/$package" --appimage-extract-and-run --version
QT_QPA_PLATFORM=offscreen "$output_dir/$package" --appimage-extract-and-run --demo --smoke-test
(
    cd "$output_dir"
    sha256sum "$package" > "$package.sha256"
)
{
    printf 'Application: VAAPI Checker %s\nArchitecture: %s\nBuild baseline: Ubuntu 24.04\n' "$version" "$arch"
    printf 'Runtime targets: Ubuntu 24.04, Debian 13\n'
    printf 'QtTest enabled: %s\n' "$run_tests"
    printf 'Host dependencies: libva.so.2, libva-drm.so.2, libdrm, Wayland libraries, GPU driver, graphics/system libraries\n'
    printf 'Optional host tools: ffmpeg, ffprobe\n\n'
    cmake --version | head -n 1
    g++ --version | head -n 1
    pkg-config --modversion Qt6Widgets libva
    printf '\nPackaging tools (verified SHA256):\n'
    awk -v arch="$arch" '$3 == arch { print $1, $2, $4 }' "$source_dir/packaging/appimage/tools.lock"
    printf '\nBuild environment packages:\n'
    dpkg-query -W -f='${Package}\t${Version}\n'
} > "$output_dir/$package.build-info.txt"
printf '\nCreated: %s\n' "$output_dir/$package"
