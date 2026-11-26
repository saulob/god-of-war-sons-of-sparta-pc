#!/bin/sh
# Builds tests/interpolation_fallback_spirv_tests.cpp with the production shader recompiler, sirit and the GPU shim's
# assertion/log/trace sources (Linux, clang++, libfmt, libZydis). Usage:
#   interpolation_spirv_build.sh <repo> <out-dir>
# SOS_OVERLAY=<dir>: a source <dir>/<repo-relative path> is compiled instead of the repository's (mutation runs); only
# .cpp files may be overlaid. Objects are cached per source content in <out-dir>/obj, so a mutation run recompiles only
# the mutated file. Nothing in the repository is written.
set -eu
repo=$1
out=$2
overlay=${SOS_OVERLAY:-}
mkdir -p "$out/obj"
cd "$repo"
includes="-Igpu/shim -Igpu/shadps4 -Isrc -Igpu/third_party/sirit/include -Igpu/third_party/sirit/src \
 -Igpu/third_party/sirit/externals/SPIRV-Headers/include -Igpu/third_party/magic_enum/include \
 -Igpu/third_party/half/include -Igpu/third_party/gcn/include -Igpu/third_party/xbyak"
flags="-std=gnu++23 -O1 -w -DNDEBUG -DBOOST_ASIO_STANDALONE -fno-strict-aliasing $includes"
pick() {
  if [ -n "$overlay" ] && [ -f "$overlay/$1" ]; then echo "$overlay/$1"; else echo "$1"; fi
}
sources=$( (find gpu/shadps4/shader_recompiler -name '*.cpp'; find gpu/third_party/sirit/src -name '*.cpp'; \
  echo gpu/shim/common/assert.cpp; echo gpu/shim/common/device_lost.cpp; echo gpu/shim/common/gpu_trace.cpp; \
  echo gpu/shim/common/logging/log.cpp; echo gpu/shadps4/common/io_file.cpp; echo gpu/shadps4/common/path_util.cpp; \
  echo gpu/shadps4/common/decoder.cpp; echo gpu/shadps4/common/signal_context.cpp; \
  echo gpu/shadps4/video_core/amdgpu/pixel_format.cpp) | sort)
for source in $sources; do
  file=$(pick "$source")
  key=$( (cat "$file"; echo "$source $flags") | sha1sum | cut -c1-16)
  echo "$file $out/obj/$key.o"
done > "$out/objects.txt"
# Compile what is missing, 8 at a time.
while read -r file object; do
  [ -f "$object" ] || echo "$file $object"
done < "$out/objects.txt" | xargs -r -P 8 -n 2 sh -c 'clang++ '"$flags"' -c "$0" -o "$1.tmp" && mv "$1.tmp" "$1" || { echo "compile failed: $0" >&2; exit 255; }'
clang++ $flags "$(pick tests/interpolation_fallback_spirv_tests.cpp)" $(awk '{print $2}' "$out/objects.txt") \
  -lfmt -lZydis -pthread -o "$out/interpolation_fallback_spirv_tests"
