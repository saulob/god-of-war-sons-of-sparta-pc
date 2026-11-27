/* The instruction sets of the port's own Windows binaries and the libraries shipped with them.
 *
 * sons_of_sparta.exe and trg-updater.exe run their startup and the processor check on any x86-64: they may hold nothing
 * beyond SSE2 but XGETBV (behind OSXSAVE), TZCNT, which processors without BMI1 run as BSF (LLVM's encoding of a
 * count whose zero case is undefined), and CMPXCHG16B, which only the cheats' code writes use, after the processor
 * check that requires it. A shipped library may use what every processor the game accepts has (AVX,
 * POPCNT, CMPXCHG16B and the SSE levels below AVX); anything newer only in a library that picks it by CPUID at run time.
 *
 *   host_isa_check FILE.exe|FILE.dll ...      exit 1 when a file breaks its rule */
#include <Zydis/Zydis.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const dispatching[] = {"SDL3.dll", "avcodec-", "avformat-", "avutil-", "swscale-", "swresample-"};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int baseline(ZydisISASet s) {
    switch (s) {
    case ZYDIS_ISA_SET_I86: case ZYDIS_ISA_SET_I186: case ZYDIS_ISA_SET_I286PROTECTED: case ZYDIS_ISA_SET_I286REAL:
    case ZYDIS_ISA_SET_I386: case ZYDIS_ISA_SET_I486: case ZYDIS_ISA_SET_I486REAL: case ZYDIS_ISA_SET_LONGMODE:
    case ZYDIS_ISA_SET_PPRO: case ZYDIS_ISA_SET_PENTIUMREAL: case ZYDIS_ISA_SET_PENTIUMMMX: case ZYDIS_ISA_SET_CMOV:
    case ZYDIS_ISA_SET_X87: case ZYDIS_ISA_SET_FCMOV: case ZYDIS_ISA_SET_FCOMI: case ZYDIS_ISA_SET_FAT_NOP:
    case ZYDIS_ISA_SET_PREFETCH_NOP: case ZYDIS_ISA_SET_PAUSE: case ZYDIS_ISA_SET_SSE: case ZYDIS_ISA_SET_SSE2:
    case ZYDIS_ISA_SET_SSE2MMX: case ZYDIS_ISA_SET_SSEMXCSR: case ZYDIS_ISA_SET_SSE_PREFETCH: case ZYDIS_ISA_SET_CLFSH:
    case ZYDIS_ISA_SET_FXSAVE: case ZYDIS_ISA_SET_FXSAVE64: case ZYDIS_ISA_SET_RDPMC:
        return 1;
    default:
        return 0;
    }
}

/* What every processor the game accepts has: AVX implies the SSE levels before it. */
static int game_minimum(ZydisISASet s) {
    switch (s) {
    case ZYDIS_ISA_SET_SSE3: case ZYDIS_ISA_SET_SSE3X87: case ZYDIS_ISA_SET_SSSE3: case ZYDIS_ISA_SET_SSSE3MMX:
    case ZYDIS_ISA_SET_SSE4: case ZYDIS_ISA_SET_SSE42: case ZYDIS_ISA_SET_POPCNT: case ZYDIS_ISA_SET_AVX:
    case ZYDIS_ISA_SET_CMPXCHG16B: case ZYDIS_ISA_SET_LAHF: case ZYDIS_ISA_SET_XSAVE:
        return 1;
    default:
        return baseline(s);
    }
}

static int allowed(const ZydisDecodedInstruction *in, int own) {
    if (in->mnemonic == ZYDIS_MNEMONIC_TZCNT || in->mnemonic == ZYDIS_MNEMONIC_XGETBV || in->mnemonic == ZYDIS_MNEMONIC_CMPXCHG16B) return 1;
    return own ? baseline(in->meta.isa_set) : game_minimum(in->meta.isa_set);
}

static int check_file(const char *path) {
    const char *name = strrchr(path, '\\');
    const char *slash = strrchr(path, '/');
    if (!name || (slash && slash > name)) name = slash;
    name = name ? name + 1 : path;
    const int own = !strcmp(name, "sons_of_sparta.exe") || !strcmp(name, "trg-updater.exe");
    int dispatch = 0;
    for (size_t i = 0; i < sizeof dispatching / sizeof *dispatching; ++i)
        dispatch |= !strncmp(name, dispatching[i], strlen(dispatching[i]));

    FILE *f = fopen(path, "rb");
    if (!f) { printf("  [fail] %s cannot be read\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *file = malloc((size_t)n);
    if (fread(file, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(file); printf("  [fail] %s cannot be read\n", path); return 0; }
    fclose(f);
    if (n < 0x200 || rd16(file) != 0x5a4d) { free(file); printf("  [fail] %s is not a PE file\n", name); return 0; }
    const uint8_t *pe = file + rd32(file + 0x3c), *opt = pe + 24, *sections = opt + rd16(pe + 20);
    if (rd16(opt) != 0x20b) { free(file); printf("  [fail] %s is not PE32+\n", name); return 0; }
    const uint32_t image_size = rd32(opt + 56);
    uint8_t *image = calloc((size_t)image_size + 16, 1), *executable = calloc((size_t)image_size + 16, 1);
    for (unsigned i = 0; i < rd16(pe + 6); ++i) {
        const uint8_t *s = sections + 40 * i;
        const uint32_t size = rd32(s + 8), rva = rd32(s + 12), raw_size = rd32(s + 16), raw = rd32(s + 20);
        const uint32_t copy = raw_size < size ? raw_size : size;
        if ((uint64_t)rva + size > image_size || (uint64_t)raw + copy > (uint64_t)n) continue;
        memcpy(image + rva, file + raw, copy);
        if (rd32(s + 36) & 0x20000000) memset(executable + rva, 1, size);
    }
    /* The functions .pdata names first; then the executable bytes it does not cover, decoded in order from the start of
     * each run (leaf functions without unwind data, and padding). */
    const uint32_t pdata = rd32(opt + 112 + 3 * 8), pdata_size = rd32(opt + 112 + 3 * 8 + 4);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t functions = 0, instructions = 0, outside = 0, reported = 0;
    uint8_t *covered = calloc((size_t)image_size + 16, 1);
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t at = 0;
        for (;;) {
            uint32_t begin, end;
            if (!pass) {
                if (!((uint64_t)pdata + at + 12 <= image_size && at + 12 <= pdata_size)) break;
                begin = rd32(image + pdata + at);
                end = rd32(image + pdata + at + 4);
                at += 12;
                if (!begin || end <= begin || end > image_size || !executable[begin]) continue;
                ++functions;
            } else {
                while (at < image_size && (!executable[at] || covered[at])) ++at;
                if (at >= image_size) break;
                begin = at;
                while (at < image_size && executable[at] && !covered[at]) ++at;
                end = at;
            }
            for (uint32_t p = begin; p < end;) {
                ZydisDecodedInstruction in;
                if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&decoder, NULL, image + p, end - p, &in))) { ++p; continue; }
                ++instructions;
                if (!pass) memset(covered + p, 1, in.length);
                if (!allowed(&in, own)) {
                    ++outside;
                    if (!dispatch && reported++ < 8)
                        printf("    %s: %s (%s) at RVA %#x%s\n", name, ZydisMnemonicGetString(in.mnemonic),
                               ZydisISASetGetString(in.meta.isa_set), p, pass ? ", outside the .pdata functions" : "");
                }
                p += in.length;
            }
        }
    }
    free(covered);
    free(image);
    free(executable);
    free(file);
    const int ok = functions > 0 && (dispatch || outside == 0);
    printf("  %s%-24s %6" PRIu64 " functions, %8" PRIu64 " instructions, %" PRIu64 " beyond %s%s\n", ok ? "" : "[fail] ",
           name, functions, instructions, outside, own ? "x86-64 (SSE2)" : "the game's minimum (AVX)",
           dispatch && outside ? ", chosen by CPUID at run time" : "");
    return ok;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;
    for (int i = 1; i < argc; ++i) failed += !check_file(argv[i]);
    return failed ? 1 : 0;
}
