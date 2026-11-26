#ifndef SOS_ISA_EMULATE_H
#define SOS_ISA_EMULATE_H
#include <stddef.h>
#include <stdint.h>

/* gpr uses x86 register numbering (rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15). Executes the MOVBE at
 * code (15 readable bytes), whose address is rip, and returns its length; 0 leaves gpr and memory untouched. */
size_t isa_emulate_movbe(const uint8_t *code, uint64_t rip, uint64_t gpr[16]);
/* The same for MOVBE and the BMI1 instructions (ANDN, BEXTR, BLSI, BLSMSK, BLSR), which also update rflags, and for the
 * AMD SSE4a instructions (EXTRQ, INSERTQ, MOVNTSD, MOVNTSS), which use xmm: the 16 registers' low 128 bits as the
 * exception context holds them (NULL: SSE4a is not emulated). Bits 127:64 of an EXTRQ or INSERTQ destination are
 * architecturally undefined and stay unchanged. */
size_t isa_emulate(const uint8_t *code, uint64_t rip, uint64_t gpr[16], uint64_t *rflags, uint8_t xmm[16][16]);

/* Writes host code for the INSERTQ with immediates at code to out, which must be the address the code will run at
 * (ISA_INSERTQ_REWRITE_MAX bytes). It needs no SSE4a and leaves rflags and every register but the destination as they
 * were; AVX only saves and restores its scratch register, in the 32 bytes below rsp (the caller keeps its red zone
 * out of them). Returns its length, 0 for any other instruction and for a field wider than 64 bits (left to
 * isa_emulate). */
enum { ISA_INSERTQ_REWRITE_MAX = 160 };
size_t isa_insertq_rewrite(const uint8_t *code, uint8_t *out);

/* VCVTPH2PS and VCVTPS2PH (F16C) in their VEX.128 forms, on the ymm registers (a register destination's bits 255:128
 * are cleared); mxcsr supplies DAZ and the rounding an immediate with bit 2 set asks for. Returns the length, 0 for
 * anything else. */
size_t isa_emulate_f16c(const uint8_t *code, uint64_t rip, const uint64_t gpr[16], uint8_t ymm[16][32], uint32_t mxcsr);
uint32_t isa_half_to_float(uint16_t half);
/* rounding: 0 nearest even, 1 down, 2 up, 3 toward zero. */
uint16_t isa_float_to_half(uint32_t single, unsigned rounding, int denormals_are_zero);

enum { ISA_EMULATE_MOVBE = 1, ISA_EMULATE_BMI1 = 2, ISA_EMULATE_SSE4A = 4 };

/* CPUID leaf 1 ECX, leaf 7 EBX and leaf 0x80000001 ECX (SSE4a is bit 6 there). */
typedef struct { uint32_t leaf1_ecx, leaf7_ebx, ext1_ecx; } IsaHostFeatures;
IsaHostFeatures isa_host_features(void);
/* ISA_EMULATE_* bits of the instruction sets isa_emulate covers that the host lacks. */
unsigned isa_emulation_needed(IsaHostFeatures host);
/* The instruction sets the game's code uses that the host lacks, comma separated in out: with handled nonzero the ones
 * the port supplies in software (MOVBE, BMI1, LZCNT, F16C, SSE4a), otherwise the ones it cannot run without (AVX,
 * POPCNT, CMPXCHG16B). Returns how many. */
int isa_missing_sets(IsaHostFeatures host, int handled, char *out, size_t size);
/* The same sets the host has. */
int isa_present_sets(IsaHostFeatures host, char *out, size_t size);
/* The CPUID brand string. */
void isa_host_name(char *out, size_t size);

#endif
