#!/usr/bin/env bash
# Builds God of War: Sons of Sparta PC on Linux x86_64 (Clang + CMake + Ninja) and runs the headless test gate.
#
#   tools/build.sh [--release] [--test] [--clean]
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }

config=Release
run_tests=0
clean=0
for arg in "$@"; do
    case "$arg" in
        --release) config=Release ;;
        --relwithdebinfo) config=RelWithDebInfo ;;
        --test) run_tests=1 ;;
        --clean) clean=1 ;;
        *) die "Unknown option: $arg" ;;
    esac
done

export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy

for tool in "$CC" "$CXX" cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || die "Missing build tool: $tool"
done

# Ensure FFmpeg is the LGPL 8.1.3 build (either already in /usr/local or built in cache/deps).
ffmpeg_args=()
if pkg-config --atleast-version=59.27 libavformat &&
   pkg-config --atleast-version=59.37 libavcodec &&
   pkg-config --atleast-version=57.28 libavutil &&
   pkg-config --atleast-version=6.7 libswscale &&
   pkg-config --atleast-version=4.7 libswresample; then
    if [[ -f /usr/local/share/licenses/ffmpeg/COPYING.LGPLv2.1 ]]; then
        echo "==> Using system-prefix LGPL FFmpeg ($(pkg-config --modversion libavcodec))"
    else
        die "Installed FFmpeg lacks /usr/local/share/licenses/ffmpeg/COPYING.LGPLv2.1"
    fi
else
    ff_ver=$(awk '{ gsub(/\r/, "") } $1=="version"{print $2}' "$root/gpu/ffmpeg.txt")
    ff_prefix="$root/cache/deps/ffmpeg/$ff_ver/linux"
    bash "$root/tools/build-ffmpeg.sh" "$ff_prefix" "$root/cache/deps/ffmpeg/$ff_ver/work"
    ffmpeg_args=("-DDCP_FFMPEG_PREFIX=$ff_prefix")
fi

build_dir="$root/build/cmake/linux-${config,,}"
if [[ $clean -eq 1 && -d $build_dir ]]; then
    rm -rf -- "$build_dir"
fi

echo "==> Configuring Linux $config build in $build_dir"
cmake -S "$root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$config" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    "${ffmpeg_args[@]}"

echo "==> Building sons_of_sparta and dcpgpu_link_smoke"
ninja -C "$build_dir" sons_of_sparta dcpgpu_link_smoke

bin="$build_dir/bin/sons_of_sparta"
[[ -x $bin ]] || die "Missing built binary $bin"
echo "  [ok] Built $bin"

if [[ $run_tests -eq 0 ]]; then
    exit 0
fi

echo "==> Running Linux headless test gate"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0
skipped=0
skip_suite() {
    echo "  [skip] $1: $2"
    skipped=$((skipped + 1))
}
run_suite() {
    local name=$1
    shift
    echo "  [run]  $name"
    if "$@"; then
        echo "  [pass] $name"
        pass=$((pass + 1))
    else
        echo "  [fail] $name"
        fail=$((fail + 1))
    fi
}

sdl_cflags=$(pkg-config --cflags sdl3)
sdl_libs=$(pkg-config --libs sdl3)
ssl_cflags=$(pkg-config --cflags openssl)
ssl_libs=$(pkg-config --libs openssl)
zlib_libs=$(pkg-config --libs zlib)

# 1. Guest entry trampoline (SysV ABI, zero RBP unwind termination, guest stack)
run_suite "guest-entry" bash -c "
    '$CC' -std=gnu11 -Wall -Wextra -Werror -O2 -fno-omit-frame-pointer -I'$root/src' -DSOS_SOURCE_DIR=$root \
        '$root/src/guest_entry.c' '$root/tests/guest_entry_tests.c' -o '$tmp/guest_entry_tests' &&
    '$tmp/guest_entry_tests'
"

# 2. POSIX SIGPIPE startup policy (live socketpair EPIPE check)
run_suite "umtx" bash -c "
    '$CC' -std=gnu11 -Wall -Wextra -O1 -pthread -I'$root/src' -I'$root/launcher' -I'$root/gpu/shim' \
        '$root/tests/umtx_tests.c' '$root/src/runtime_umtx.c' -o '$tmp/umtx_tests' &&
    '$tmp/umtx_tests'
"

run_suite "posix-sigpipe" bash -c "
    '$CC' -std=gnu11 -Wall -Wextra -Werror -O2 -I'$root/src' -DSOS_SOURCE_DIR=$root \
        '$root/tests/sigpipe_tests.c' -o '$tmp/posix_sigpipe_tests' &&
    '$tmp/posix_sigpipe_tests'
"

# 3. ISA emulation (MOVBE, BMI1, SSE4a): the test drives the Windows exception path.
skip_suite "isa-emulate" "Windows-only test, covered by tools/test.ps1"

# 4. Redzone analysis: the test drives the Windows exception path.
skip_suite "redzone-analysis" "Windows-only test, covered by tools/test.ps1"

# 5. Runtime hot path recursive lock
if [[ -f /usr/include/spdlog/spdlog.h || -f /usr/local/include/spdlog/spdlog.h ]]; then
    run_suite "runtime-hot-path" bash -c "
        '$CXX' -std=c++23 -Wall -Wextra -Werror -O2 -pthread -I'$root/gpu/shadps4' -I'$root/gpu/shim' \
            -DSOS_SOURCE_DIR=$root '$root/gpu/shadps4/common/recursive_lock.cpp' \
            '$root/tests/recursive_lock_tests.cpp' -o '$tmp/hot_path_tests' &&
        '$tmp/hot_path_tests'
    "
else
    skip_suite "runtime-hot-path" "spdlog headers are not installed; covered by tools/test.ps1"
fi

# 6. Sampler lifetime collector
run_suite "sampler-lifetime" bash -c "
    '$CXX' -std=c++23 -Wall -Wextra -O1 -I'$root/gpu/shim' -I'$root/gpu/shadps4' -I'$root/src' \
        -I'$root/gpu/third_party/half/include' -I'$root/gpu/third_party/magic_enum/include' \
        -DSOS_SOURCE_DIR=$root '$root/tests/sampler_lifetime_tests.cpp' -o '$tmp/sampler_tests' &&
    '$tmp/sampler_tests'
"

# 7. Low VRAM allocation fallback policy
run_suite "memory-fallback" bash -c "
    '$CXX' -std=c++23 -Wall -Wextra -O1 -I'$root/gpu/shim' -I'$root/gpu/shadps4' -I'$root/src' \
        -I'$root/gpu/third_party/half/include' -I'$root/gpu/third_party/magic_enum/include' \
        -DSOS_SOURCE_DIR=$root '$root/tests/memory_fallback_tests.cpp' -o '$tmp/mem_fallback_tests' &&
    '$tmp/mem_fallback_tests'
"

# 8. Frame pacing limiter
run_suite "frame-pacing" bash -c "
    '$CXX' -std=c++23 -Wall -Wextra -O1 -I'$root/gpu/shim' -I'$root/gpu/shadps4' -I'$root/src' -I'$root/launcher' \
        -DSOS_SOURCE_DIR=$root '$root/tests/frame_pacing_tests.cpp' -o '$tmp/frame_pacing_tests' &&
    '$tmp/frame_pacing_tests'
"

# 9. Presentation swapchain state machine
run_suite "presentation-swapchain" bash -c "
    '$CXX' -std=c++23 -Wall -Wextra -O1 -I'$root/gpu/shim' -I'$root/gpu/shadps4' -I'$root/src' \
        -I'$root/gpu/third_party/half/include' -I'$root/gpu/third_party/magic_enum/include' \
        -DSOS_SOURCE_DIR=$root '$root/tests/presentation_swapchain_tests.cpp' -o '$tmp/swapchain_tests' &&
    '$tmp/swapchain_tests'
"

# 10. SaveData mount diagnostics: the test drives the Windows file layer.
skip_suite "save-mount-diagnostics" "Windows-only test, covered by tools/test.ps1"
run_suite "save-backup" bash -c "
    mkdir -p '$tmp/save_backup_user' &&
    '$CC' -std=gnu11 -Wall -Wextra -O1 -pthread -I'$root/src' -I'$root/launcher' -I'$root/gpu/shim' \
        '$root/tests/savedata_backup_tests.c' '$root/src/runtime_savedata.c' -o '$tmp/save_backup_tests' &&
    SOS_TEST_SAVE_DIR='$tmp/save_backup_user' '$tmp/save_backup_tests'
"

# 11. Launcher headless unit tests (linked against real built sos_launcher_core)
run_suite "launcher-core" bash -c "
    '$CC' -std=gnu11 -Wall -Wextra -Werror -O1 -c '$root/src/sos_hermes_unlock.c' -o '$tmp/sos_hermes_unlock.o' &&
    '$CC' -std=gnu11 -Wall -Wextra -Werror -O1 -c '$root/src/sos_pit_unlock.c' -o '$tmp/sos_pit_unlock.o' &&
    '$CXX' -std=c++20 -Wall -Wextra -O1 -no-pie -DSOS_SOURCE_DIR='\"$root\"' -DSOS_PORT_VERSION='\"0.1.0\"' \
        -I'$root/launcher' -I'$root/src' -I'$root/third_party/trg-launcher/include' $sdl_cflags $ssl_cflags \
        '$root/tests/launcher_tests.cpp' '$tmp/sos_hermes_unlock.o' '$tmp/sos_pit_unlock.o' \
        '$build_dir/launcher/libsos_launcher_core.a' '$build_dir/launcher/libtrg_core.a' \
        '$build_dir/libsos_keys.a' '$build_dir/libsos_startup_phase.a' \
        $sdl_libs $ssl_libs $zlib_libs -pthread -ldl -o '$tmp/launcher_tests' &&
    '$tmp/launcher_tests'
"

# 12. Window lifecycle static checks (shutdown, display, cursor)
if command -v python3 >/dev/null; then
    for area in shutdown display cursor; do
        run_suite "lifecycle-$area" python3 -I "$root/scripts/window_lifecycle_check.py" --area "$area"
    done
    run_suite "graphics-state-source" python3 -I "$root/scripts/graphics_state_check.py"
fi

echo "==> Linux headless test summary: PASS=$pass FAIL=$fail SKIPPED=$skipped"
[[ $fail -eq 0 ]]
