#!/usr/bin/env bash
# Mirrors a Windows working tree into ~/.cache/god-of-war-sons-of-sparta-pc-build inside Ubuntu 20.04 WSL,
# runs tools/build.sh --release --test and tools/package-linux.sh, and copies dist/ artifacts back
# (SonsOfSpartaPC-v<version>-x86_64.AppImage + .sha256; Windows counterpart is SonsOfSpartaPC-v<version>-windows-x64.zip).
set -euo pipefail
src_repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
mirror="${SOS_WSL_MIRROR:-$HOME/.cache/god-of-war-sons-of-sparta-pc-build}"

command -v rsync >/dev/null || { echo "ERROR: rsync is required" >&2; exit 1; }

echo "==> Mirroring repository to $mirror"
mkdir -p "$mirror"
rsync -a --checksum --no-times --delete \
    --exclude='.git/' \
    --exclude='build/' \
    --exclude='dist/' \
    --exclude='game/' \
    --exclude='user/' \
    --exclude='cache/' \
    --exclude='logs/' \
    --exclude='.venv/' \
    "$src_repo/" "$mirror/"

chmod +x "$mirror/tools/"*.sh "$mirror/packaging/linux/AppRun"

echo "==> Building Linux Release and running headless tests"
bash "$mirror/tools/build.sh" --release --test

echo "==> Packaging Linux x86_64 AppImage"
bash "$mirror/tools/package-linux.sh"

version=$(sed -nE 's/^project\s*\(\s*sons_of_sparta_pc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$mirror/CMakeLists.txt" | tr -d '\r')
app_name="SonsOfSpartaPC-v${version}-x86_64"
app_file="SonsOfSpartaPC-v${version}-x86_64.AppImage"

echo "==> Copying Linux release artifacts back to $src_repo/dist"
mkdir -p "$src_repo/dist"
cp -f "$mirror/dist/${app_file}" "$src_repo/dist/"
cp -f "$mirror/dist/${app_file}.sha256" "$src_repo/dist/"
rm -rf -- "$src_repo/dist/${app_name}-symbols"
cp -a "$mirror/dist/${app_name}-symbols" "$src_repo/dist/"

(
    cd "$src_repo/dist"
    sha256sum -c "${app_file}.sha256"
)
echo "==> Linux release packaging complete"
