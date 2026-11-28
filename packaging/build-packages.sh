#!/usr/bin/env bash

set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=$(sed -nE 's/^project\(CompositorLX VERSION ([0-9.]+).*/\1/p' "$source_dir/CMakeLists.txt")
architecture=$(dpkg --print-architecture)
appimage_arch=$(uname -m)
output_dir="$source_dir/artifacts"
work_dir=$(mktemp -d /tmp/compositorlx-package-XXXXXX)
tool_dir=$(mktemp -d /tmp/compositorlx-tools-XXXXXX)
app_dir=$(mktemp -d /tmp/compositorlx-appdir-XXXXXX)

cleanup() {
    rm -rf -- "$work_dir" "$tool_dir" "$app_dir"
}
trap cleanup EXIT

for command_name in cmake cpack curl ninja qmake6; do
    command -v "$command_name" >/dev/null || {
        echo "Missing required command: $command_name" >&2
        exit 1
    }
done

# ONNXRUNTIME_ARCHIVE: a local copy of the pinned ONNX Runtime archive, when its download is unavailable (hash-checked).
cmake -S "$source_dir" -B "$work_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release ${ONNXRUNTIME_ARCHIVE:+"-DONNXRUNTIME_ARCHIVE=$ONNXRUNTIME_ARCHIVE"}
cmake --build "$work_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
OMP_NUM_THREADS="${OMP_NUM_THREADS:-4}" ctest --test-dir "$work_dir" --output-on-failure

mkdir -p "$output_dir"
(cd "$work_dir" && cpack -G DEB)
cp "$work_dir"/*.deb "$output_dir/compositorlx_${version}_${architecture}.deb"

curl -fL --retry 3 \
    -o "$tool_dir/linuxdeploy-x86_64.AppImage" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -fL --retry 3 \
    -o "$tool_dir/linuxdeploy-plugin-qt-x86_64.AppImage" \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x "$tool_dir"/*.AppImage

DESTDIR="$app_dir" cmake --install "$work_dir" --prefix /usr
# mktemp makes the AppDir 0700; appimagetool refuses an AppDir others could not read and run.
chmod -R a+rX "$app_dir"
# A Qt Multimedia from CMAKE_PREFIX_PATH isn't where the loader looks: linuxdeploy is told where it is.
multimedia_cmake=$(sed -nE 's/^Qt6Multimedia_DIR:PATH=(.*)/\1/p' "$work_dir/CMakeCache.txt")
multimedia_libdir=${multimedia_cmake:+$(cd "$multimedia_cmake/../.." && pwd)}
original_library_path=${LD_LIBRARY_PATH-}
export LD_LIBRARY_PATH="${multimedia_libdir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
(
    cd "$work_dir"
    EXTRA_PLATFORM_PLUGINS="libqoffscreen.so${EXTRA_PLATFORM_PLUGINS:+;$EXTRA_PLATFORM_PLUGINS}" \
    QMAKE=$(command -v qmake6) "$tool_dir/linuxdeploy-x86_64.AppImage" \
        --appimage-extract-and-run \
        --appdir "$app_dir" \
        --desktop-file "$source_dir/packaging/compositor-lx.desktop" \
        --icon-file "$source_dir/packaging/icons/compositor-lx-512.png" \
        --plugin qt
)
# Movie sound: Qt Multimedia's FFmpeg backend, from where the build's Qt Multimedia is (it may not be among the system's
# plugins, where linuxdeploy-plugin-qt looks). It links Qt Quick for its QML video sinks, which LX never loads, so it is
# added after the Qt plugin has run (which would otherwise deploy QML) and only its libraries are bundled.
multimedia_plugin=$(sed -nE 's/.*COMPOSITOR_DEV_PLUGIN_DIR=\\?"([^"\\]+).*/\1/p' "$work_dir/build.ninja" | head -n1)
for plugins in "$multimedia_plugin" "$(qmake6 -query QT_INSTALL_PLUGINS)"; do
    if [[ -n "$plugins" && -f "$plugins/multimedia/libffmpegmediaplugin.so" ]]; then
        mkdir -p "$app_dir/usr/plugins/multimedia"
        cp "$plugins/multimedia/libffmpegmediaplugin.so" "$app_dir/usr/plugins/multimedia/"
        chmod -R a+rX "$app_dir/usr/plugins/multimedia"
        break
    fi
done
# appimagetool makes the AppImage's own icon (.DirIcon) from a PNG named for the desktop file's Icon at the AppDir's top.
cp "$source_dir/packaging/icons/compositor-lx-256.png" "$app_dir/compositor-lx.png"
chmod a+r "$app_dir/compositor-lx.png"
(
    cd "$work_dir"
    "$tool_dir/linuxdeploy-x86_64.AppImage" \
        --appimage-extract-and-run \
        --appdir "$app_dir" \
        --desktop-file "$source_dir/packaging/compositor-lx.desktop" \
        --icon-file "$source_dir/packaging/icons/compositor-lx-512.png" \
        ${multimedia_plugin:+--deploy-deps-only "$app_dir/usr/plugins/multimedia"} \
        --output appimage
)
# The smoke tests below run the AppImage as a user would, with only what it bundles.
if [[ -n "$original_library_path" ]]; then export LD_LIBRARY_PATH="$original_library_path"; else unset LD_LIBRARY_PATH; fi
cp "$work_dir/CompositorLX-${appimage_arch}.AppImage" \
    "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage"
chmod +x "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage"

QT_QPA_PLATFORM=offscreen "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage" \
    --appimage-extract-and-run --check-subject-model
QT_QPA_PLATFORM=offscreen "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage" \
    --appimage-extract-and-run --version
QT_QPA_PLATFORM=offscreen "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage" \
    --appimage-extract-and-run --check-renderer
QT_QPA_PLATFORM=offscreen VK_DRIVER_FILES=/nonexistent/compositor-icd.json \
    VK_ICD_FILENAMES=/nonexistent/compositor-icd.json \
    "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage" \
    --appimage-extract-and-run --check-renderer
dpkg-deb --info "$output_dir/compositorlx_${version}_${architecture}.deb" >/dev/null
(
    cd "$output_dir"
    sha256sum "CompositorLX-${version}-${appimage_arch}.AppImage" \
        "compositorlx_${version}_${architecture}.deb" > SHA256SUMS
    sha256sum --check SHA256SUMS
)
