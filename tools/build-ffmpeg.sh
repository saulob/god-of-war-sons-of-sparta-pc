#!/usr/bin/env bash
# Builds the FFmpeg pinned in gpu/ffmpeg.txt into PREFIX. tools\build.ps1 runs it in MSYS2 CLANG64 and tools/build.sh on
# Linux. Nothing is rebuilt while PREFIX holds the unchanged build of the current pin and script.
#
#   tools/build-ffmpeg.sh PREFIX [WORK]     WORK keeps the downloaded tarball (default: a temporary folder)
#   tools/build-ffmpeg.sh --check PREFIX    fails unless PREFIX holds that build
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }
to_unix() { if command -v cygpath >/dev/null; then cygpath -u "$1"; else printf '%s' "$1"; fi; }

check_only=0
if [[ ${1:-} == --check ]]; then check_only=1; shift; fi
prefix=$(to_unix "${1:?usage: tools/build-ffmpeg.sh [--check] PREFIX [WORK]}")
[[ $prefix == /* ]] || die "PREFIX must be an absolute path: $prefix"
prefix=$(realpath -m -- "$prefix")
pin=$root/gpu/ffmpeg.txt
field() { awk -v key="$1" '{ gsub(/\r/, "") } $1 == key { $1 = ""; sub(/^ /, ""); print }' "$pin"; }
version=$(field version) url=$(field url) sha256=$(field sha256)
[[ -n $version && $url == https://* && $sha256 =~ ^[0-9a-f]{64}$ ]] || die "$pin needs version, an https url and sha256"
case $(uname -s) in
    MINGW*|MSYS*) platform=windows ;;
    Linux) platform=linux ;;
    *) die "Unsupported system: $(uname -s)" ;;
esac
cc=${CC:-clang}
read -r -a options <<<"$(field configure) $(field "$platform")"

stamp=$prefix/share/licenses/ffmpeg/dcp-build.stamp
libraries=$prefix/share/licenses/ffmpeg/dcp-libs.sha256
sha256_of() { sha256sum <"$1" | cut -d' ' -f1; }
expected=$(sha256_of "$pin")-$(sha256_of "${BASH_SOURCE[0]}")
is_current() {
    [[ -f $stamp && $(<"$stamp") == "$expected" && -s $libraries ]] &&
        (cd "$prefix" && sha256sum -c --status "$libraries" 2>/dev/null)
}
if is_current; then
    echo "FFmpeg $version is current in $prefix"
    exit 0
fi
((check_only)) && die "$prefix does not hold the FFmpeg $version of gpu/ffmpeg.txt and tools/build-ffmpeg.sh"

# PREFIX is replaced as a whole: only a folder of an earlier build, or an empty one, may be.
[[ $prefix != / && "$root/" != "$prefix"/* ]] || die "PREFIX must not contain the repository: $prefix"
if [[ -e $prefix && ! -f $stamp ]] && { [[ ! -d $prefix ]] || [[ -n $(ls -A -- "$prefix") ]]; }; then
    die "$prefix holds files tools/build-ffmpeg.sh did not install"
fi
for tool in curl tar xz make nasm sha256sum "$cc"; do
    command -v "$tool" >/dev/null || die "Missing $tool"
done

if [[ -n ${2:-} ]]; then
    work=$(to_unix "$2")
    [[ $work == /*/* ]] || die "WORK must be an absolute folder below the root: $work"
    mkdir -p "$work"
else
    work=$(mktemp -d)
    trap '[[ $work == /*/* ]] && rm -rf -- "$work"' EXIT
fi
tarball=$work/ffmpeg-$version.tar.xz
if ! echo "$sha256  $tarball" | sha256sum -c --status - 2>/dev/null; then
    echo "Fetching FFmpeg $version"
    curl -fsSL --retry 3 -o "$tarball.part" "$url"
    echo "$sha256  $tarball.part" | sha256sum -c --status - ||
        { rm -f "$tarball.part"; die "$url does not match the SHA-256 in gpu/ffmpeg.txt"; }
    mv -f "$tarball.part" "$tarball"
fi
# A scanner can briefly hold files of an interrupted build: retry the removal, then stop rather than mix old and new sources.
discard() {
    local attempt
    [[ $1 == /*/* ]] || die "Refusing to remove $1"
    for attempt in 1 2 3 4 5; do
        rm -rf -- "$1" 2>/dev/null
        [[ -e $1 ]] || return 0
        sleep 2
    done
    die "Could not remove $1"
}
discard "$work/build"
discard "$work/install"
src=$work/build/src
mkdir -p "$src"
tar -xJf "$tarball" -C "$src" --strip-components=1
[[ -f $src/configure && -f $src/LICENSE.md ]] || die "$tarball did not extract into $src"
cd "$work/build"

echo "Building FFmpeg $version ($platform) into $prefix"
# The libraries carry their configure line and source file names. Sources in ./src make configure use the relative
# path "src", and a neutral prefix keeps the install folder out of them.
src/configure --prefix=/ffmpeg --cc="$cc" "${options[@]}" >configure.log 2>&1 ||
    { tail -40 configure.log >&2; die 'FFmpeg configure failed'; }
grep -qx 'License: LGPL version 2.1 or later' configure.log || die 'FFmpeg was not configured as LGPL 2.1 or later'
make -j"$(nproc)" >make.log 2>&1 || { tail -40 make.log >&2; die 'FFmpeg build failed'; }
make install DESTDIR="$work/install" >install.log 2>&1 || { tail -20 install.log >&2; die 'FFmpeg install failed'; }
staged=$work/install/ffmpeg
# pkgconf and clang are Windows programs there: they need the prefix as S:/...
pc_prefix=$prefix
needles=("$work" "$prefix")
if [[ $platform == windows ]]; then
    pc_prefix=$(cygpath -m "$prefix")
    needles+=("$(cygpath -m "$work")" "$pc_prefix")
fi
sed -i "s|=/ffmpeg|=$pc_prefix|g" "$staged"/lib/pkgconfig/libav*.pc "$staged"/lib/pkgconfig/libsw*.pc
! grep -qE '(=|-[IL])/ffmpeg(/|$)' "$staged"/lib/pkgconfig/*.pc || die 'pkg-config files still name /ffmpeg'
built=()
for lib in "$staged"/bin/av*.dll "$staged"/bin/sw*.dll "$staged"/lib/*.dll.a "$staged"/lib/libav*.so.* "$staged"/lib/libsw*.so.*; do
    [[ -f $lib && ! -L $lib ]] || continue
    built+=("${lib#"$staged"/}")
    for needle in "${needles[@]}"; do
        ! grep -aqF -- "$needle" "$lib" || die "$lib names the folder $needle"
    done
done
((${#built[@]} >= 5)) || die "FFmpeg installed only ${built[*]:-no libraries}"
licenses=$staged/share/licenses/ffmpeg
mkdir -p "$licenses"
cp -f "$src/LICENSE.md" "$src/COPYING.LGPLv2.1" "$licenses/"
printf '%s\n%s\n' "FFmpeg $version, $url (SHA-256 $sha256)" "./configure ${options[*]}" >"$licenses/BUILD.txt"
(cd "$staged" && sha256sum -- "${built[@]}") >"$licenses/dcp-libs.sha256"
printf '%s' "$expected" >"$licenses/dcp-build.stamp"

mkdir -p "$(dirname -- "$prefix")"
discard "$prefix.new"
cp -a "$staged" "$prefix.new"
discard "$prefix"
mv -- "$prefix.new" "$prefix"
discard "$work/install"
is_current || die "$prefix does not match the build just installed"
echo "FFmpeg $version installed in $prefix"
