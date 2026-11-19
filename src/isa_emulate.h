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

enum { ISA_EMULATE_MOVBE = 1, ISA_EMULATE_BMI1 = 2, ISA_EMULATE_SSE4A = 4 };

/* CPUID leaf 1 ECX, leaf 7 EBX and leaf 0x80000001 ECX (SSE4a is bit 6 there). */
typedef struct { uint32_t leaf1_ecx, leaf7_ebx, ext1_ecx; } IsaHostFeatures;
IsaHostFeatures isa_host_features(void);
/* ISA_EMULATE_* bits of the instruction sets isa_emulate covers that the host lacks. */
unsigned isa_emulation_needed(IsaHostFeatures host);
/* The Jaguar instruction sets the host lacks, comma separated in out: the ones isa_emulate works around when emulated
 * is nonzero, otherwise the ones the game cannot run without. Returns how many. */
int isa_missing_sets(IsaHostFeatures host, int emulated, char *out, size_t size);
/* The CPUID brand string. */
void isa_host_name(char *out, size_t size);

#endif
