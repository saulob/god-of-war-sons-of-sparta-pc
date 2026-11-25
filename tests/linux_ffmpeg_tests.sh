#!/usr/bin/env bash
# Which FFmpeg a Linux build uses: the real tools/build.sh, tools/build-ffmpeg.sh and gpu/cmake/PinnedFFmpeg.cmake in a
# temporary project, with a small stand-in FFmpeg and a decoy FFmpeg on the pkg-config path.
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
temp=${1:?usage: tests/linux_ffmpeg_tests.sh TEMP_FOLDER}
[[ $temp == /*/* && ! -e $temp ]] || { echo "TEMP_FOLDER must be a new absolute folder: $temp" >&2; exit 2; }
mkdir -p "$temp"
export CC=${CC:-clang} CXX=${CXX:-clang++}
failures=0
check() { # name command...
    local name=$1
    shift
    if "$@"; then echo "  [ok  ] $name"; else echo "  [FAIL] $name"; failures=$((failures + 1)); fi
}
not() { ! "$@"; }

make_tarball() { # file
    local src=$temp/source/ffmpeg
    mkdir -p "$src"
    cat >"$src/configure" <<'EOF'
#!/bin/sh
echo "License: LGPL version 2.1 or later"
printf 'all:\ninstall:\n\tsh src/install.sh "$(DESTDIR)/ffmpeg" /ffmpeg "%s"\n' "$*" >Makefile
EOF
    cat >"$src/install.sh" <<'EOF'
set -e
mkdir -p "$1/lib/pkgconfig" "$1/include"
for item in avformat:62 avcodec:62 avutil:60 swscale:9 swresample:6; do
    name=${item%:*} major=${item#*:}
    printf 'const char *%s_configuration(void) { return "%s"; }\n' "$name" "$3" >"$name.c"
    "${CC:-clang}" -shared -fPIC -Wl,-soname,"lib$name.so.$major" -o "$1/lib/lib$name.so.$major.0.0" "$name.c"
    ln -sf "lib$name.so.$major.0.0" "$1/lib/lib$name.so.$major"
    ln -sf "lib$name.so.$major" "$1/lib/lib$name.so"
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: %s\nDescription: stand-in\nVersion: %s.0.0\nLibs: -L${libdir} -l%s\nCflags: -I${includedir}\n' \
        "$2" "$name" "$major" "$name" >"$1/lib/pkgconfig/lib$name.pc"
done
EOF
    chmod +x "$src/configure"
    echo license >"$src/LICENSE.md"
    echo license >"$src/COPYING.LGPLv2.1"
    tar -cJf "$1" -C "$temp/source" ffmpeg
}

project=$temp/project
deps=$project/cache/deps/ffmpeg/9.9.9
prefix=$deps/linux
pinned=$project/tools/build-ffmpeg.sh
build_dir=$project/build/cmake/linux-release
mkdir -p "$project/tools" "$project/gpu/cmake" "$deps/work"
cp "$root/tools/build.sh" "$root/tools/build-ffmpeg.sh" "$project/tools/"
cp "$root/gpu/cmake/PinnedFFmpeg.cmake" "$project/gpu/cmake/"
make_tarball "$deps/work/ffmpeg-9.9.9.tar.xz"
write_pin() { # configure options
    printf 'version 9.9.9\nurl https://example.invalid/ffmpeg.tar.xz\nsha256 %s\nconfigure %s\nlinux --enable-pthreads\n' \
        "$(sha256sum <"$deps/work/ffmpeg-9.9.9.tar.xz" | cut -d' ' -f1)" "$1" >"$project/gpu/ffmpeg.txt"
}
write_pin --first
cat >"$project/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(sons_of_sparta_pc VERSION 9.8.7 LANGUAGES C CXX)
find_package(PkgConfig REQUIRED)
include(gpu/cmake/PinnedFFmpeg.cmake)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach (target sons_of_sparta dcpgpu_link_smoke)
    add_executable(${target} main.c)
    target_link_libraries(${target} PRIVATE PkgConfig::FFMPEG)
endforeach()
EOF
cat >"$project/main.c" <<'EOF'
const char *avformat_configuration(void), *avcodec_configuration(void), *avutil_configuration(void),
    *swscale_configuration(void), *swresample_configuration(void);
int main(void) {
    return !avformat_configuration() + !avcodec_configuration() + !avutil_configuration() + !swscale_configuration() + !swresample_configuration();
}
EOF

decoy=$temp/decoy
mkdir -p "$decoy/share/licenses/ffmpeg"
(cd "$temp" && sh "$temp/source/ffmpeg/install.sh" "$decoy" "$decoy" decoy)
echo license >"$decoy/share/licenses/ffmpeg/COPYING.LGPLv2.1"
export PKG_CONFIG_PATH=$decoy/lib/pkgconfig

run() { output=$("$@" 2>&1); code=$?; }
build() { run bash "$project/tools/build.sh" --release; }
says() { [[ $(tr -s '[:space:]' ' ' <<<"$output") == *"$1"* ]]; } # CMake wraps its messages
succeeded() { [[ $code -eq 0 ]] || { printf '%s\n' "$output" | tail -15; return 1; }; }
uses_pinned() {
    local count
    count=$(ldd "$build_dir/bin/sons_of_sparta" | awk -v dir="$prefix/lib/" '$1 ~ /^lib(av|sw)/ && index($3, dir) == 1' | wc -l)
    [[ $count -eq 5 ]] && ! grep -q "$decoy" "$build_dir/build.ninja"
}
pinned_current() { bash "$pinned" --check "$prefix" >/dev/null 2>&1; }
libraries_name() { grep -raqF -- "$1" "$prefix"/lib/*.so.*; }

check 'the decoy is what pkg-config finds by itself' test "$(pkg-config --variable=prefix libavcodec)" = "$decoy"
check 'nothing is accepted before a build' not pinned_current
build
check 'a fresh configuration builds' succeeded
check 'it builds the pinned FFmpeg first' says "Building FFmpeg 9.9.9"
check 'it links the pinned FFmpeg although another one is installed' uses_pinned
check 'it names the five libraries it resolved' test "$(grep -c '\[ffmpeg\] lib' <<<"$output")" -eq 5
check 'the libraries name neither the prefix nor the project' not libraries_name "$project"
check 'the libraries carry the neutral prefix' libraries_name '--prefix=/ffmpeg'
check 'the pkg-config files name the real prefix' grep -qx "prefix=$prefix" "$prefix/lib/pkgconfig/libavcodec.pc"
before=$(stat -c %Y.%i "$prefix/lib/libavcodec.so.62.0.0")
build
check 'a second build succeeds' succeeded
check 'it keeps the current FFmpeg' test "$before" = "$(stat -c %Y.%i "$prefix/lib/libavcodec.so.62.0.0")"
check 'it compiles and links nothing' says 'no work to do'

poison_cache() {
    local args=() name
    for name in avformat avcodec avutil swscale swresample; do
        args+=("-Dpkgcfg_lib_FFMPEG_$name:FILEPATH=$decoy/lib/lib$name.so")
    done
    sed -i -e "s|^DCP_FFMPEG_PREFIX:PATH=.*|DCP_FFMPEG_PREFIX:PATH=$decoy|" \
        -e "s|^\(pkgcfg_lib_FFMPEG_\([a-z]*\):FILEPATH\)=.*|\1=$decoy/lib/lib\2.so|" \
        -e "s|^\(FFMPEG_[A-Za-z_]*:INTERNAL\)=$prefix|\1=$decoy|" "$build_dir/CMakeCache.txt"
    grep -q "^pkgcfg_lib_FFMPEG_avcodec:FILEPATH=$decoy" "$build_dir/CMakeCache.txt"
}
check 'the CMake cache can be made to name the decoy' poison_cache
build
check 'a build over that cache succeeds' succeeded
check 'the stale cache does not choose the FFmpeg' uses_pinned
check 'the cache names the pinned libraries again' grep -q "^pkgcfg_lib_FFMPEG_avcodec:FILEPATH=$prefix/" "$build_dir/CMakeCache.txt"

stamp=$prefix/share/licenses/ffmpeg/dcp-build.stamp
rebuilds() { # reason
    check "$1: the prefix is not accepted" not pinned_current
    build
    check "$1: FFmpeg is rebuilt" says 'Building FFmpeg 9.9.9'
    check "$1: the build succeeds on the pinned FFmpeg" uses_pinned
    check "$1: the prefix is current afterwards" pinned_current
}
rm "$stamp"
check 'a prefix without its stamp is not replaced blindly' not bash "$pinned" "$prefix" "$deps/work" 2>/dev/null
check 'its files are still there' test -f "$prefix/lib/libavcodec.so.62.0.0"
printf wrong >"$stamp"
rebuilds 'a mismatched stamp'
printf x >>"$prefix/lib/libswscale.so.9.0.0"
rebuilds 'a changed library'
rm "$prefix/lib/libavutil.so.60.0.0"
rebuilds 'a missing library'
rm "$prefix/share/licenses/ffmpeg/dcp-libs.sha256"
rebuilds 'a missing library list'
echo '# changed' >>"$pinned"
rebuilds 'a changed build-ffmpeg.sh'
write_pin --second
rebuilds 'a changed pin'
check 'the new configuration reached the libraries' libraries_name --second
check 'BUILD.txt records the new configuration' grep -q -- --second "$prefix/share/licenses/ffmpeg/BUILD.txt"

configure() { # folder arguments...
    local dir=$temp/configure/$1
    shift
    run cmake -S "$project" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" "$@"
}
refused() { [[ $code -ne 0 ]] && says "$1"; }
configure fresh "-DDCP_FFMPEG_PREFIX=$prefix"
check 'CMake names the pinned libraries it selected' says "Pinned FFmpeg: $prefix/lib/libavformat.so"
configure none
check 'CMake refuses to configure without a prefix' refused 'DCP_FFMPEG_PREFIX is not set'
configure decoy "-DDCP_FFMPEG_PREFIX=$decoy"
check 'CMake refuses an unstamped FFmpeg' refused 'holds no FFmpeg built by'
printf wrong >"$stamp"
configure stamp "-DDCP_FFMPEG_PREFIX=$prefix"
check 'CMake refuses a mismatched stamp' refused 'is not the one of gpu/ffmpeg.txt'
build
check 'the build repairs it' uses_pinned
cp -a "$prefix/lib/pkgconfig/libavcodec.pc" "$temp/libavcodec.pc"
sed -i "s|^prefix=.*|prefix=$decoy|" "$prefix/lib/pkgconfig/libavcodec.pc"
configure redirected "-DDCP_FFMPEG_PREFIX=$prefix"
check 'CMake refuses a library outside the prefix' refused "FFmpeg was found in $decoy"
cp -a "$temp/libavcodec.pc" "$prefix/lib/pkgconfig/libavcodec.pc"
run env LD_LIBRARY_PATH="$decoy/lib" bash "$project/tools/build.sh" --release
check 'a binary that resolves another FFmpeg is refused' refused 'not to the pinned FFmpeg'

mkdir -p "$temp/foreign"
echo keep >"$temp/foreign/file"
run bash "$pinned" "$temp/foreign"
check 'a folder with other files is refused' refused 'did not install'
check 'and kept' test -f "$temp/foreign/file"
for unsafe in / "$project" "$temp" relative/prefix; do
    run bash "$pinned" "$unsafe"
    check "the prefix $unsafe is refused" test "$code" -ne 0
done
check 'the project is intact' test -f "$project/CMakeLists.txt"

if ((failures)); then echo "$failures FFmpeg check(s) failed"; exit 1; fi
echo 'Linux FFmpeg selection passed'
