#!/usr/bin/env bash
# Packages the Linux x86_64 Release build into dist/SonsOfSpartaPC-v<version>-x86_64.AppImage (+ .sha256).
# The Windows counterpart is dist/SonsOfSpartaPC-v<version>-windows-x64.zip (+ .sha256) built by tools/package.ps1.
# Enforces Ubuntu 20.04 / glibc 2.31 baseline, DT_RPATH '$ORIGIN/../lib', usr/lib/compat fallback,
# LGPL FFmpeg verification, forbidden-file & local-path scanning, pinned appimagetool 1.9.1 and
# reviewed static musl type-2 runtime SHA-256, and headless --appimage-extract round-trip verification.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }

version=$(sed -nE 's/^project\s*\(\s*sons_of_sparta_pc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/CMakeLists.txt" | tr -d '\r')
[[ -n $version ]] || die "Could not read VERSION from CMakeLists.txt"

app_name="SonsOfSpartaPC-v${version}-x86_64"
app_file="SonsOfSpartaPC-v${version}-x86_64.AppImage"
build_dir="${SOS_LINUX_BUILD_DIR:-$root/build/cmake/linux-release}"
bin="$build_dir/bin/sons_of_sparta"
dist_dir="$root/dist"
appdir="$root/build/appimage/AppDir"
symbols_dir="$dist_dir/${app_name}-symbols"
out_appimage="$dist_dir/${app_file}"
out_sha="${out_appimage}.sha256"

APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
source "$root/patches/appimage-runtime/baseline.env"

for tool in readelf objcopy patchelf ldd sha256sum diff dpkg-query; do
    command -v "$tool" >/dev/null || die "Missing packaging tool: $tool"
done
[[ -x $bin ]] || die "Missing Linux Release binary: $bin (run tools/build.sh --release first)"

# Verify host glibc baseline <= 2.31
host_glibc=$(ldd --version | head -n1 | grep -oE '[0-9]+\.[0-9]+$' || true)
echo "==> Host glibc: ${host_glibc:-unknown} (required <= 2.31)"
if [[ -n $host_glibc ]]; then
    host_major=${host_glibc%%.*}
    host_minor=${host_glibc#*.}
    if (( host_major > 2 || (host_major == 2 && host_minor > 31) )); then
        die "Host glibc $host_glibc exceeds Ubuntu 20.04 baseline 2.31"
    fi
fi

# Verify binary is x86_64 and non-PIE
elf_hdr=$(readelf -h "$bin")
grep -q 'Machine:.*Advanced Micro Devices X86-64' <<<"$elf_hdr" || die "$bin is not x86_64"
grep -q 'Type:.*EXEC' <<<"$elf_hdr" || die "$bin must be non-PIE EXEC (guest heap/TLS below 1 TiB)"

check_glibc_max() {
    local elf=$1
    local bad
    bad=$(readelf --version-info -W "$elf" 2>/dev/null \
        | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
        | sort -u \
        | awk -F'[_.]' '$2 > 2 || ($2 == 2 && $3 > 31) { print }' || true)
    if [[ -n $bad ]]; then
        die "$elf requires glibc newer than 2.31: $(echo "$bad" | tr '\n' ' ')"
    fi
}

# Libraries provided by the host OS / GPU driver / display server (never bundled).
is_host_lib() {
    case "$1" in
        linux-vdso.so*|ld-linux-*.so*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libnsl.so.*|libcrypt.so.*|libanl.so.*)
            return 0 ;;
        libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libEGL.so.*|libGLESv2.so.*|libvulkan.so.*|libdrm*.so.*|libgbm.so.*|libwayland-*.so.*)
            return 0 ;;
        libX11.so.*|libX11-xcb.so.*|libXext.so.*|libXau.so.*|libXdmcp.so.*|libxcb*.so.*|libXfixes.so.*|libXcursor.so.*|libXrandr.so.*|libXi.so.*|libXinerama.so.*|libXss.so.*|libXxf86vm.so.*|libxkbcommon*.so.*)
            return 0 ;;
        libasound.so.*|libpulse*.so.*|libpipewire-*.so.*|libdbus-1.so.*|libudev.so.*|libsystemd.so.*|libfontconfig.so.*|libexpat.so.*|libuuid.so.*|libgpg-error.so.*|libgcrypt.so.*|liblz4.so.*|liblzma.so.*|libz.so.*)
            return 0 ;;
        *) return 1 ;;
    esac
}

echo "==> Staging AppDir ($appdir)"
rm -rf -- "$appdir" "$symbols_dir"
mkdir -p "$appdir/usr/bin/assets/launcher" \
         "$appdir/usr/lib/compat" \
         "$appdir/usr/share/applications" \
         "$appdir/usr/share/metainfo" \
         "$appdir/usr/share/icons/hicolor/256x256/apps" \
         "$appdir/usr/share/doc/sons-of-sparta-pc/licenses" \
         "$symbols_dir" "$dist_dir"

cp -f "$bin" "$appdir/usr/bin/sons_of_sparta"
objcopy --only-keep-debug "$appdir/usr/bin/sons_of_sparta" "$symbols_dir/sons_of_sparta.debug"
objcopy --strip-debug --add-gnu-debuglink="$symbols_dir/sons_of_sparta.debug" "$appdir/usr/bin/sons_of_sparta" || \
    objcopy --strip-debug "$appdir/usr/bin/sons_of_sparta"
chmod 0755 "$appdir/usr/bin/sons_of_sparta"

cp -f "$root/assets/launcher/header-art.png" "$appdir/usr/bin/assets/launcher/header-art.png"
cp -f "$root/assets/launcher/sponsor-heart.png" "$appdir/usr/bin/assets/launcher/sponsor-heart.png"
cp -f "$root/packaging/README.txt" "$appdir/usr/share/doc/sons-of-sparta-pc/README.txt"
cp -f "$root/LICENSE" "$appdir/usr/share/doc/sons-of-sparta-pc/LICENSE.txt"
cp -f "$root/packaging/licenses/THIRD-PARTY-LICENSES.txt" "$appdir/usr/share/doc/sons-of-sparta-pc/licenses/THIRD-PARTY-LICENSES.txt"
cp -R "$root/packaging/licenses/components" "$appdir/usr/share/doc/sons-of-sparta-pc/licenses/"
cp -f "$root/gpu/VENDOR.txt" "$appdir/usr/share/doc/sons-of-sparta-pc/licenses/VENDOR.txt"

# Bundle FFmpeg LGPL licenses
for ff_lic in /usr/local/share/licenses/ffmpeg "$root/cache/deps/ffmpeg/"*/linux/share/licenses/ffmpeg; do
    if [[ -d $ff_lic ]]; then
        mkdir -p "$appdir/usr/share/doc/sons-of-sparta-pc/licenses/ffmpeg"
        cp -f "$ff_lic"/{LICENSE.md,COPYING.LGPLv2.1,BUILD.txt} "$appdir/usr/share/doc/sons-of-sparta-pc/licenses/ffmpeg/" 2>/dev/null || true
        break
    fi
done

# Bundle non-host shared libraries (transitive ldd closure)
echo "==> Collecting shared library closure"
declare -A seen=()
declare -A lib_owner=()
queue=("$appdir/usr/bin/sons_of_sparta")
while (( ${#queue[@]} > 0 )); do
    cur="${queue[0]}"
    queue=("${queue[@]:1}")
    while IFS= read -r line; do
        [[ $line != *"not found"* ]] || die "unresolved library of $cur: $line"
        [[ $line == *"=>"* ]] || continue
        soname=$(awk '{print $1}' <<<"$line")
        target=$(awk '{print $3}' <<<"$line")
        [[ $target == /* && -f $target ]] || continue
        [[ -z ${seen[$soname]:-} ]] || continue
        seen[$soname]=1
        if is_host_lib "$soname"; then
            continue
        fi
        real=$(readlink -f -- "$target")
        lib_owner[$soname]=$real
        if [[ $soname == libstdc++.so.* || $soname == libgcc_s.so.* ]]; then
            cp -f "$real" "$appdir/usr/lib/compat/$soname"
            chmod 0644 "$appdir/usr/lib/compat/$soname"
            echo "  [compat] $soname <= $real"
            continue
        fi
        cp -f "$real" "$appdir/usr/lib/$soname"
        chmod 0644 "$appdir/usr/lib/$soname"
        echo "  [lib]    $soname <= $real"
        queue+=("$appdir/usr/lib/$soname")
    done < <(ldd "$cur" 2>/dev/null || true)
done

# Verify LGPL FFmpeg libraries
for fflib in "$appdir/usr/lib"/libav*.so.* "$appdir/usr/lib"/libsw*.so.*; do
    [[ -f $fflib ]] || continue
    grep -aq 'LGPL version 2.1 or later' "$fflib" || die "$fflib is not LGPL 2.1+"
    ! grep -aq -- '--enable-gpl' "$fflib" || die "$fflib has --enable-gpl"
done

# License texts of the bundled shared libraries: the Ubuntu package that owns the library, or the
# component text staged above for a library built in /usr/local.
licenses="$appdir/usr/share/doc/sons-of-sparta-pc/licenses"
manifest="$licenses/SYSTEM-PACKAGES.txt"
{
    echo "Libraries bundled in the AppImage, with the packages or sources they come from and their license texts."
    echo "Libraries and drivers the player's system provides (glibc, Vulkan, X11/Wayland, audio) are not bundled."
} >"$manifest"
for soname in "${!lib_owner[@]}"; do
    real=${lib_owner[$soname]}
    package=$(dpkg-query -S "$real" 2>/dev/null | head -1 | cut -d: -f1 || true)
    if [[ -z $package && $real == /usr/lib/* ]]; then
        package=$(dpkg-query -S "/lib/${real#/usr/lib/}" 2>/dev/null | head -1 | cut -d: -f1 || true)
    fi
    if [[ -n $package && -f /usr/share/doc/$package/copyright ]]; then
        mkdir -p "$licenses/system/$package"
        cp -f "/usr/share/doc/$package/copyright" "$licenses/system/$package/copyright"
        printf '\n%s (%s)\n  License text: licenses/system/%s/copyright\n' "$soname" "$package" "$package" >>"$manifest"
        continue
    fi
    case "${soname%%.so*}" in
        libSDL3) component=SDL3 ;;
        libfmt) component=fmt ;;
        libZydis|libZycore) component=Zydis ;;
        libxxhash) component=xxHash ;;
        libcrypto|libssl) component=OpenSSL ;;
        libunwind) component=libunwind ;;
        libavcodec|libavformat|libavutil|libswscale|libswresample) component=FFmpeg ;;
        *) component= ;;
    esac
    if [[ $component == FFmpeg && -f $licenses/ffmpeg/COPYING.LGPLv2.1 ]]; then
        printf '\n%s (FFmpeg, built from the pinned source)\n  License text: licenses/ffmpeg/\n' "$soname" >>"$manifest"
    elif [[ -n $component && -d $licenses/components/$component ]]; then
        mkdir -p "$licenses/system/$component"
        cp -R "$licenses/components/$component/." "$licenses/system/$component/"
        printf '\n%s (%s, built from source)\n  License text: licenses/system/%s/\n' "$soname" "$component" "$component" >>"$manifest"
    else
        die "No license text recorded for bundled $soname ($real)"
    fi
done

# Enforce DT_RPATH (not RUNPATH) and strip debug sections on bundled libs
# patchelf 0.10 keeps an existing RUNPATH unless it is removed first.
patchelf --remove-rpath "$appdir/usr/bin/sons_of_sparta"
patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$appdir/usr/bin/sons_of_sparta"
for lib in "$appdir/usr/lib"/*.so*; do
    [[ -f $lib ]] || continue
    patchelf --remove-rpath "$lib" || true
    patchelf --force-rpath --set-rpath '$ORIGIN' "$lib" || true
done

# Compute GLIBCXX / GCC minimum versions needed for AppRun compat fallback
glibcxx_need=$(readelf --version-info -W "$appdir/usr/bin/sons_of_sparta" "$appdir/usr/lib"/*.so* 2>/dev/null \
    | grep -oE 'GLIBCXX_3\.4\.[0-9]+' | sed 's/GLIBCXX_3\.4\.//' | sort -n | tail -1 || true)
gcc_need=$(readelf --version-info -W "$appdir/usr/bin/sons_of_sparta" "$appdir/usr/lib"/*.so* 2>/dev/null \
    | grep -oE 'GCC_[0-9]+\.[0-9]+' | sed 's/^GCC_//' | cut -d. -f1 | sort -n | tail -1 || true)
glibcxx_need=${glibcxx_need:-28}
gcc_need=${gcc_need:-4}

sed -e "s/@GLIBCXX_NEED@/$glibcxx_need/g" -e "s/@GCC_NEED@/$gcc_need/g" \
    "$root/packaging/linux/AppRun" >"$appdir/AppRun"
chmod 0755 "$appdir/AppRun"

cp -f "$root/packaging/linux/io.github.saulob.sons_of_sparta_pc.desktop" \
      "$appdir/io.github.saulob.sons_of_sparta_pc.desktop"
cp -f "$root/packaging/linux/io.github.saulob.sons_of_sparta_pc.desktop" \
      "$appdir/usr/share/applications/io.github.saulob.sons_of_sparta_pc.desktop"

release_date=$(date -u +%Y-%m-%d)
sed -e "s/@VERSION@/$version/g" -e "s/@DATE@/$release_date/g" \
    "$root/packaging/linux/io.github.saulob.sons_of_sparta_pc.appdata.xml" \
    >"$appdir/usr/share/metainfo/io.github.saulob.sons_of_sparta_pc.appdata.xml"

cp -f "$root/packaging/icons/app-icon-256.png" "$appdir/sons-of-sparta-pc.png"
cp -f "$root/packaging/icons/app-icon-256.png" "$appdir/.DirIcon"
cp -f "$root/packaging/icons/app-icon-256.png" "$appdir/usr/share/icons/hicolor/256x256/apps/sons-of-sparta-pc.png"

check_appdir() {
    local dir=$1
    [[ -x "$dir/AppRun" ]] || die "AppRun missing or not executable"
    [[ -x "$dir/usr/bin/sons_of_sparta" ]] || die "usr/bin/sons_of_sparta missing"
    [[ -f "$dir/usr/bin/assets/launcher/header-art.png" ]] || die "header-art.png missing"
    [[ -f "$dir/usr/bin/assets/launcher/sponsor-heart.png" ]] || die "sponsor-heart.png missing"
    [[ ! -e "$dir/usr/bin/assets/launcher/header-art-source.png" ]] || die "header-art-source.png must not ship"
    [[ -f "$dir/io.github.saulob.sons_of_sparta_pc.desktop" ]] || die "desktop entry missing"
    [[ -f "$dir/sons-of-sparta-pc.png" ]] || die "icon missing"
    [[ -f "$dir/usr/share/doc/sons-of-sparta-pc/README.txt" ]] || die "README.txt missing"
    [[ -f "$dir/usr/share/doc/sons-of-sparta-pc/LICENSE.txt" ]] || die "LICENSE.txt missing"
    [[ -f "$dir/usr/share/doc/sons-of-sparta-pc/licenses/THIRD-PARTY-LICENSES.txt" ]] || die "THIRD-PARTY-LICENSES.txt missing"
    [[ -f "$dir/usr/share/doc/sons-of-sparta-pc/licenses/components/Boost/LICENSE_1_0.txt" ]] || die "component license texts missing"
    [[ -s "$dir/usr/share/doc/sons-of-sparta-pc/licenses/SYSTEM-PACKAGES.txt" ]] || die "bundled-library license manifest missing"

    local rpath_out
    rpath_out=$(readelf -d "$dir/usr/bin/sons_of_sparta")
    grep -qE '\(RPATH\).*\[\$ORIGIN/\.\./lib\]' <<<"$rpath_out" || die "sons_of_sparta lacks DT_RPATH \$ORIGIN/../lib"
    ! grep -q '(RUNPATH)' <<<"$rpath_out" || die "sons_of_sparta has DT_RUNPATH instead of DT_RPATH"

    local elf
    while IFS= read -r -d '' elf; do
        check_glibc_max "$elf"
        if grep -aq 'DEBUG_SAULO' "$elf"; then die "DEBUG_SAULO marker in $elf"; fi
        if grep -aq 'VisualStudio' "$elf"; then die "Local Windows path in $elf"; fi
    done < <(find "$dir/usr/bin" "$dir/usr/lib" -maxdepth 2 -type f \( -name 'sons_of_sparta' -o -name '*.so*' \) -print0)

    # Ensure no forbidden files (game data, saves, logs, renderdoc, debug symbols)
    local bad_file
    bad_file=$(find "$dir" -type f \( -name '*.pkg' -o -name '*.self' -o -name '*.prx' -o -name '*.sfo' -o -name '*.dmp' -o -name '*.pdb' -o -name '*.debug' -o -name '*.log' -o -name 'eboot.bin' -o -name 'boot.bin' -o -name 'sons_of_sparta.ini' -o -name '*renderdoc*' -o -name 'VkLayer_*' \) | head -n1 || true)
    [[ -z $bad_file ]] || die "Forbidden file in AppDir: $bad_file"
}

echo "==> Verifying AppDir"
check_appdir "$appdir"

# Locate pinned appimagetool 1.9.1 and reviewed static musl type-2 runtime
locate_by_sha() {
    local expected=$1
    shift
    for cand in "$@"; do
        [[ -f $cand ]] || continue
        local got
        got=$(sha256sum "$cand" | awk '{print $1}')
        if [[ $got == "$expected" ]]; then
            printf '%s' "$cand"
            return 0
        fi
    done
    return 1
}

appimagetool=$(locate_by_sha "$APPIMAGETOOL_SHA256" \
    "$root/build/appimage/appimagetool-1.9.1-x86_64.AppImage" \
    "$HOME/.cache/alienation-pc-build/build/appimage/appimagetool-1.9.1-x86_64.AppImage" \
    "$HOME/.cache/13-sentinels-aegis-rim-pc-build/build/appimage/appimagetool-1.9.1-x86_64.AppImage" \
    "$HOME/.cache/dragons-crown-pro-pc-build/build/appimage/appimagetool-1.9.1-x86_64.AppImage" || true)
[[ -n $appimagetool ]] || die "Pinned appimagetool 1.9.1 (SHA-256 $APPIMAGETOOL_SHA256) not found"

runtime_bin=$(locate_by_sha "$RUNTIME_SHA256" \
    "$root/build/appimage/runtime-sos-x86_64" \
    "$HOME/.cache/alienation-pc-build/build/appimage/runtime-aln-x86_64" \
    "$HOME/.cache/13-sentinels-aegis-rim-pc-build/build/appimage/runtime-tsr-x86_64" \
    "$HOME/.cache/dragons-crown-pro-pc-build/build/appimage/runtime-dcp-x86_64" || true)
[[ -n $runtime_bin ]] || die "Pinned type-2 AppImage runtime (SHA-256 $RUNTIME_SHA256) not found"

echo "==> Packaging AppImage with verified appimagetool and pinned runtime"
rm -f -- "$out_appimage" "$out_sha"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$appimagetool" --no-appstream --runtime-file "$runtime_bin" "$appdir" "$out_appimage"
chmod 0755 "$out_appimage"

# Verify embedded runtime header & type-2 magic ('AI\x02' at offset 8)
magic=$(od -An -tx1 -j8 -N3 "$out_appimage" | tr -d ' \n')
[[ $magic == "414902" ]] || die "AppImage lacks type-2 magic bytes (got $magic)"

# Headless --appimage-extract round-trip verification (never runs AppRun or game!)
echo "==> Verifying AppImage extraction round-trip (headless)"
extract_tmp=$(mktemp -d)
trap 'rm -rf "$extract_tmp"' EXIT
(
    cd "$extract_tmp"
    "$out_appimage" --appimage-extract >/dev/null
)
check_appdir "$extract_tmp/squashfs-root"
diff -r --no-dereference "$appdir" "$extract_tmp/squashfs-root" >/dev/null || die "Extracted AppImage differs from AppDir"

(
    cd "$dist_dir"
    sha256sum "$(basename -- "$out_appimage")" >"$(basename -- "$out_sha")"
    sha256sum -c "$(basename -- "$out_sha")"
)
echo "  [ok] Packaged $out_appimage ($(du -h "$out_appimage" | awk '{print $1}'))"
