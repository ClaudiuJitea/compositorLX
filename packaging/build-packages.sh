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

cmake -S "$source_dir" -B "$work_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$work_dir" --parallel "$(nproc)"
ctest --test-dir "$work_dir" --output-on-failure

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
(
    cd "$work_dir"
    QMAKE=$(command -v qmake6) "$tool_dir/linuxdeploy-x86_64.AppImage" \
        --appimage-extract-and-run \
        --appdir "$app_dir" \
        --desktop-file "$source_dir/packaging/compositor-lx.desktop" \
        --icon-file "$source_dir/packaging/icons/compositor-lx-512.png" \
        --plugin qt \
        --output appimage
)
cp "$work_dir/CompositorLX-${appimage_arch}.AppImage" \
    "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage"
chmod +x "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage"

"$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage" \
    --appimage-extract-and-run --check-subject-model
dpkg-deb --info "$output_dir/compositorlx_${version}_${architecture}.deb" >/dev/null
sha256sum "$output_dir/CompositorLX-${version}-${appimage_arch}.AppImage" \
    "$output_dir/compositorlx_${version}_${architecture}.deb"
