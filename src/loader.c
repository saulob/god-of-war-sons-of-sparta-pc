/* Native x86-64 loader: maps the prepared PS4 boot image, resolves its imports and runs the
 * original code on the host CPU. The launcher installs/selects the game first; -CheckImports keeps the headless
 * diagnostic without the guest entry. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "runtime.h"
#include <pthread.h>
#include <unistd.h>
#include "runtime_log.h"
#include "startup_phase.h"
#include "startup_profile.h"
#include "game_build.h"
#include "dcpgpu.h"
#include "sos_launcher.h"
#include "skip_intro.h"
#include "sos_skip_intro.h"
#include "controller_prompts.h"
#include "prompt_dialog.h"
#include "trophies/trophy_runtime.h"
#include "sos_image_format.h"
#include "isa_emulate.h"
#ifdef _WIN32
#include "crash_guard.h"
#endif
#include "sos_posix_signals.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error God of War Sons of Sparta PC requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <signal.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static uint64_t image_bytes;
static unsigned char *import_resolved;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); runtime_exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low = runtime_low_map(size, PROT_READ | PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, const uintptr_t *regs) {
    if (id >= import_count) fail("bad import trap index");
    const char *name = names[id];
    const char *hash = strrchr(name, '#');
    const char *library = hash ? hash + 1 : "unknown";
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", name, id);
    printf("API: %s\n", runtime_import_name(name));
    printf("Library: %s\n", library);
    printf("Guest registers: rdi=0x%" PRIxPTR " rsi=0x%" PRIxPTR " rdx=0x%" PRIxPTR
           " rcx=0x%" PRIxPTR " r8=0x%" PRIxPTR " r9=0x%" PRIxPTR "\n",
           regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]);
    printf("First argument: 0x%" PRIxPTR " (RDI)\n", regs[0]);
    uintptr_t caller = (uintptr_t)__builtin_return_address(0) - (uintptr_t)image;
    int in_module = 0;
    for (uint64_t m = 0; m < module_count; ++m)
        if (caller >= modules[m].base && caller - modules[m].base < modules[m].size) {
            printf("Caller in linked module %" PRIu64 ": +0x%" PRIxPTR "\n", m, caller - modules[m].base);
            in_module = 1;
        }
    if (!in_module && caller < image_bytes) printf("Caller at image+0x%" PRIxPTR "\n", caller);
    printf("Thread: host %lu, guest %d (%s)\n", sos_thread_id(), runtime_thread_id_current(),
           runtime_thread_current_name() ? runtime_thread_current_name() : "unknown");
    printf("Startup phase: %s\n", sos_startup_phase_current());
    printf("Last checkpoint: %s\n", sos_checkpoint_last());
    char graphics[256];
    dcpgpu_first_frame_summary(graphics, sizeof(graphics));
    printf("Graphics progress: %s\n", graphics);
    unsigned same_library = 0;
    if (import_resolved)
        for (uint64_t i = 0; i < import_count; ++i)
            if (!import_resolved[i] && strstr(names[i], hash ? hash : "#")) ++same_library;
    printf("Unsupported import family: %s\n", library);
    printf("Remaining unresolved in family: %u\n", same_library);
    unsigned listed = 0;
    if (import_resolved)
        for (uint64_t i = 0; i < import_count && listed < 16; ++i)
            if (!import_resolved[i] && strstr(names[i], hash ? hash : "#")) {
                printf("  unresolved: %s\n", runtime_import_name(names[i]));
                ++listed;
            }
    if (same_library > listed) printf("  ... %u more\n", same_library - listed);
    runtime_history_dump();
    runtime_report();
    puts("Game initialization is incomplete; a system library this milestone does not implement was called.");
    runtime_log_flush();
    _exit(20);
}
/* A diagnostic dump when startup stalls before the first VideoOut open. Never changes behavior. */
static void *stall_watchdog(void *unused) {
    (void)unused;
    for (int seconds = 0; seconds < 15; ++seconds) {
        sleep(1);
        char graphics[256];
        dcpgpu_first_frame_summary(graphics, sizeof(graphics));
        if (!strstr(graphics, "VideoOut=0")) return NULL;
    }
    printf("Startup: stalled 15 s before VideoOut; phase %s\n", sos_startup_phase_current());
    runtime_thread_diag_dump();
    runtime_sema_diag_dump();
    runtime_signal_diag_dump();
    return NULL;
}
static void start_stall_watchdog(void) {
    runtime_thread_track_waits(1);
    pthread_t thread;
    if (!pthread_create(&thread, NULL, stall_watchdog, NULL)) pthread_detach(thread);
}
static ABI void guest_exit(void) {
    puts("Runtime: process finalizer callback reached");
    runtime_log_exit_request("guest_exit",0,(uintptr_t)__builtin_return_address(0));
}
#ifndef _WIN32
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}

static char module_names[16][128];
static char module_paths[16][512];

static void log_cpu(void) {
    char name[64], sets[96];
    const IsaHostFeatures host = isa_host_features();
    isa_host_name(name, sizeof(name));
    printf("CPU: %s\n", name);
    if (isa_missing_sets(host, 1, sets, sizeof(sets))) printf("CPU: emulating %s\n", sets);
    if (isa_missing_sets(host, 0, sets, sizeof(sets))) printf("CPU: missing %s, which cannot be emulated\n", sets);
}

#ifdef _WIN32
static int gpu_enabled;
static uint64_t image_size;
static int current_module = -1;

static void crash_detail(EXCEPTION_POINTERS *info) {
    fprintf(stderr, "  startup phase: %s\n", sos_startup_phase_current());
    fprintf(stderr, "  last checkpoint: %s\n", sos_checkpoint_last());
    char graphics[256];
    dcpgpu_first_frame_summary(graphics, sizeof(graphics));
    fprintf(stderr, "  graphics progress: %s\n", graphics);
    {
        static char operations[3072];
        const int fault_address = info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                                  info->ExceptionRecord->NumberParameters >= 2;
        if (runtime_memory_recent(operations, sizeof(operations),
                                  fault_address ? (uintptr_t)info->ExceptionRecord->ExceptionInformation[1] : 0))
            fputs(operations, stderr);
    }
    runtime_history_dump();
    if (current_module >= 0 && (uint64_t)current_module < module_count) {
        const uint64_t m = (uint64_t)current_module;
        fprintf(stderr, "  linked module initializer: %llu %s entry 0x%llx (image+0x%llx)\n",
                (unsigned long long)m, module_names[m][0] ? module_names[m] : "(unnamed)",
                (unsigned long long)((uintptr_t)image + modules[m].init), (unsigned long long)modules[m].init);
    }
}

static int crash_describe(char *out, size_t size, uintptr_t address) {
    if (!image || address < (uintptr_t)image || address - (uintptr_t)image >= image_size) return 0;
    const uint64_t offset = address - (uintptr_t)image;
    for (uint64_t m = 0; m < module_count; ++m) {
        if (offset >= modules[m].base && offset - modules[m].base < modules[m].size) {
            snprintf(out, size, "%s (linked module %llu) image+0x%llx module+0x%llx",
                     module_names[m][0] ? module_names[m] : "(unnamed)", (unsigned long long)m,
                     (unsigned long long)offset, (unsigned long long)(offset - modules[m].base));
            return 1;
        }
    }
    snprintf(out, size, "guest image+0x%llx", (unsigned long long)offset);
    return 1;
}

static void abort_signal(int sig) {
    (void)sig;
    sos_crash_guard_report_here("abort() was called: an uncaught C++ exception, a pure virtual call or a failed library check",
                                0x40000015u, 3);
}

static void purecall(void) { sos_crash_guard_report_here("pure virtual call", 0x40000015u, 3); }

/* MinGW makes a failed C runtime parameter check non-fatal (the call fails with EINVAL); the first ones are logged. */
static void invalid_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *fl, unsigned line, uintptr_t r) {
    static volatile LONG count;
    (void)e; (void)f; (void)fl; (void)line; (void)r;
    if (InterlockedIncrement(&count) > 8) return;
    void *frames[4];
    char text[512], where[160];
    int length = snprintf(text, sizeof(text), "Diagnostics: an invalid C runtime parameter (non-fatal), called from:");
    const USHORT n = RtlCaptureStackBackTrace(1, 4, frames, NULL);
    for (USHORT i = 0; i < n && length > 0 && (size_t)length < sizeof(text) - 170; ++i) {
        runtime_describe_guest_address((uintptr_t)frames[i], where, sizeof(where));
        length += snprintf(text + length, sizeof(text) - (size_t)length, "%s%s", i ? ", " : " ", where);
    }
    snprintf(text + length, sizeof(text) - (size_t)length, "\n");
    runtime_log_write_direct(text);
}

static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const uintptr_t fault_address = (uintptr_t)record->ExceptionInformation[1];
        if (runtime_memory_wait_carve(fault_address) ||
            runtime_memory_commit_fault(fault_address, (int)record->ExceptionInformation[0]))
            return EXCEPTION_CONTINUE_EXECUTION;
        const int64_t cost = sos_cost_begin();
        if (gpu_enabled && dcpgpu_handle_fault(info, (void *)record->ExceptionInformation[1])) {
            sos_cost_end(SOS_COST_GPU_FAULT, cost, 1);
            runtime_redzone_after_fault(info->ContextRecord);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (runtime_fault_recover) {
            unsigned long long *buffer = *runtime_fault_recover;
            runtime_fault_recover = NULL;
            CONTEXT *c = info->ContextRecord;
            c->Rsp = ((c->Rsp - 128) & ~(DWORD64)15) - 40;
            c->Rcx = (DWORD64)(uintptr_t)buffer;
            c->Rdx = 1;
            c->Rip = (DWORD64)(uintptr_t)sos_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION && image &&
        ((uintptr_t)info->ContextRecord->Rip - (uintptr_t)image < image_size ||
         runtime_redzone_contains((uintptr_t)info->ContextRecord->Rip))) {
        /* Jaguar's MOVBE, BMI1 and SSE4a trap on hosts without them. */
        CONTEXT *c = info->ContextRecord;
        _Static_assert(offsetof(CONTEXT, R15) - offsetof(CONTEXT, Rax) == 15 * sizeof(DWORD64), "CONTEXT register order");
        _Static_assert(sizeof(c->FltSave.XmmRegisters) == 16 * 16, "CONTEXT xmm layout");
        uint64_t flags = c->EFlags;
        uint8_t (*const xmm)[16] = (c->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT
                                       ? (uint8_t (*)[16])c->FltSave.XmmRegisters : NULL;
        const int64_t cost = sos_cost_begin();
        const size_t length = isa_emulate((const uint8_t *)(uintptr_t)c->Rip, c->Rip, &c->Rax, &flags, xmm);
        if (length) {
            sos_cost_end(SOS_COST_EMULATED, cost, 1);
            c->EFlags = (DWORD)flags;
            c->Rip += length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return sos_crash_guard_vectored(info);
}

/* Guest code reads its TCB with `mov rax, gs:[0]` (the boot image builder rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    uint32_t displacement = runtime_thread_tcb_offset();
    uint64_t patched = 0;
    for (uint64_t s = 0; s < count; ++s) {
        if (!(segments[s].flags & 1) || segments[s].size < sizeof(load)) continue;
        unsigned char *at = image + segments[s].address, *end = at + segments[s].size - sizeof(load);
        for (; at <= end; ++at)
            if (at[0] == 0x65 && !memcmp(at, load, sizeof(load))) {
                memcpy(at + 5, &displacement, 4);
                runtime_redzone_add_volatile((uint64_t)(at + 5 - image), 4);
                ++patched;
                at += sizeof(load) - 1;
            }
    }
    return patched;
}
#endif

static void load_module_names(const char *boot) {
    char image_json[4200];
    snprintf(image_json, sizeof(image_json), "%s", boot);
    char *slash = strrchr(image_json, '\\');
    char *slash2 = strrchr(image_json, '/');
    if (slash2 > slash) slash = slash2;
    if (!slash) strcpy(image_json, "image.json");
    else strcpy(slash + 1, "image.json");
    FILE *f = fopen(image_json, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *text = len > 0 ? malloc((size_t)len + 1) : NULL;
    if (text && fread(text, 1, (size_t)len, f) == (size_t)len) {
        text[len] = 0;
        const char *at = strstr(text, "\"modules\"");
        for (int m = 0; at && m < 16; ++m) {
            at = strstr(at, "\"file\"");
            if (!at) break;
            at = strchr(at, ':');
            if (!at) break;
            at = strchr(at, '"');
            if (!at) break;
            const char *start = ++at;
            const char *end = strchr(start, '"');
            if (!end) break;
            const char *base = start;
            for (const char *p = start; p < end; ++p) if (*p == '/' || *p == '\\') base = p + 1;
            size_t base_size = (size_t)(end - base);
            if (base_size >= sizeof(module_names[m])) base_size = sizeof(module_names[m]) - 1;
            memcpy(module_names[m], base, base_size);
            module_names[m][base_size] = 0;
            if (base == start)
                snprintf(module_paths[m], sizeof(module_paths[m]), "sce_module/%.*s", (int)(end - start), start);
            else
                snprintf(module_paths[m], sizeof(module_paths[m]), "%.*s", (int)(end - start), start);
            at = end + 1;
        }
    }
    free(text);
    fclose(f);
}

static const char *module_label(uint64_t m) {
    return m < 16 && module_names[m][0] ? module_names[m] : "(unnamed)";
}
static const char *module_path(uint64_t m) {
    return m < 16 && module_paths[m][0] ? module_paths[m] : NULL;
}

static int has_import_relocation(uint64_t target, const Reloc *relocs, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i)
        if (relocs[i].kind && relocs[i].target == target) return 1;
    return 0;
}

/* Only the values these sites hold change with the render resolution; the instructions a shape writes do not. */
static void note_render_target_values(const SosGameBuild *build, const SosRenderTarget *target) {
    const int height = sos_target_needs_height_sites(target);
    for (size_t i = 0; i < build->resolution_site_count; ++i) {
        const SosResolutionSite *site = &build->resolution_sites[i];
        if (site->kind == SOS_SITE_WIDTH) {
            runtime_redzone_add_volatile(site->offset, 4);
            runtime_redzone_add_volatile(site->offset + 4, 4);
        } else if (site->kind == SOS_SITE_HEIGHT_CALC && height) {
            runtime_redzone_add_volatile(site->offset + 2, 4);
        }
    }
}

/* Unity's Il2cppUserAssemblies keeps direct `jmp rel32` stubs for a few imports (the scePthreadSem*
 * family), prelinked to the console's system-library addresses. They bypass the GOT, so the normal
 * relocations never reach them. Rewrite each as `jmp *slot(%rip)` to the import's GOT entry. */
static uint64_t patch_import_stubs(const Reloc *relocs, uint64_t count) {
    uint64_t patched = 0;
    for (uint64_t m = 0; m < module_count; ++m) {
        uint64_t previous_offset = 0, previous_got = 0;
        int have_previous = 0;
        const uint64_t limit = modules[m].size < 0x20000 ? modules[m].size : 0x20000;
        for (uint64_t off = 8; off + 32 <= limit; off += 16) {
            unsigned char *at = image + modules[m].base + off;
            if (at[0] == 0xff && at[1] == 0x25) {
                int32_t disp;
                memcpy(&disp, at + 2, 4);
                previous_offset = off;
                previous_got = off + 6 + (int64_t)disp;
                have_previous = 1;
                continue;
            }
            if (at[0] != 0xe9 || at[5] != 0x07 || !have_previous || off != previous_offset + 16) continue;
            unsigned char *next = at + 16;
            if (next[0] != 0xff || next[1] != 0x25) continue;
            int32_t next_disp;
            memcpy(&next_disp, next + 2, 4);
            const uint64_t slot = previous_got + 8;
            const uint64_t next_got = off + 16 + 6 + (int64_t)next_disp;
            if (next_got != slot + 8 || !has_import_relocation(modules[m].base + slot, relocs, count)) continue;
            const int32_t disp = (int32_t)(slot - (off + 6));
            at[0] = 0xff;
            at[1] = 0x25;
            memcpy(at + 2, &disp, 4);
            printf("Import stub: module %" PRIu64 " image+0x%" PRIx64 " -> GOT image+0x%" PRIx64 "\n", m,
                   modules[m].base + off, modules[m].base + slot);
            ++patched;
        }
    }
    return patched;
}


static void load_content_profile(const char *path) {
    if (!path || !path[0]) fail("content profile path is unavailable");
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot open the content profile");
    unsigned char data[SOS_CONTENT_PROFILE_SIZE];
    const size_t got = fread(data, 1, sizeof(data), f);
    const int extra = fgetc(f);
    fclose(f);
    uint32_t values[SOS_CONTENT_VALUES];
    const SosContentStatus status = sos_content_profile_parse(data, got, values);
    if (extra != EOF) fail("content profile has trailing data");
    if (status == SOS_CONTENT_BAD_SIZE) fail("content profile has the wrong size");
    if (status == SOS_CONTENT_BAD_SIGNATURE) fail("content profile has the wrong signature");
    if (status == SOS_CONTENT_BAD_SKU) fail("content profile has an unsupported SKU");
    runtime_content_configure(values);
    printf("Content: profile %s (SKU %u, parameters %u %u %u %u)\n", path, values[0],
           values[1], values[2], values[3], values[4]);
}
static void report_image(const char *boot) {
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    printf("[run] %s\n[run] boot image %s\n", stamp, boot);
    char image_json[4200];
    snprintf(image_json, sizeof(image_json), "%s", boot);
    char *slash = strrchr(image_json, '\\');
    char *slash2 = strrchr(image_json, '/');
    if (slash2 > slash) slash = slash2;
    if (!slash) strcpy(image_json, "image.json");
    else strcpy(slash + 1, "image.json");
    FILE *jf = fopen(image_json, "rb");
    if (!jf) return;
    char *text = NULL; long len = 0;
    fseek(jf, 0, SEEK_END); len = ftell(jf); rewind(jf);
    text = malloc((size_t)len + 1);
    if (text && fread(text, 1, (size_t)len, jf) == (size_t)len) {
        text[len] = 0;
        const char *known = "unknown";
        for (size_t b = 0; b < sizeof(sos_game_builds) / sizeof(*sos_game_builds); ++b) {
            char key[96];
            snprintf(key, sizeof(key), "\"%s\"", sos_game_builds[b].eboot_sha256);
            if (strstr(text, key)) { known = sos_game_builds[b].label; break; }
        }
        printf("Build: %s\n", known);
        const char *hash = strstr(text, "\"eboot_sha256\":");
        if (hash) {
            hash = strchr(hash + 15, '"');
            if (hash) {
                const char *end = strchr(hash + 1, '"');
                if (end && end - (hash + 1) == 64)
                    printf("Eboot SHA-256: %.*s\n", 64, hash + 1);
            }
        }
    }
    free(text);
    fclose(jf);
}

/* Import classes for bring-up. The importer evidence says which boot-linked module references the symbol; the library
 * says which subsystem owns it. Graphics/audio/input/network/VR are deferred; everything unproven stays deferred or
 * optional, never assumed to be an unused optional. */
typedef enum { CLASS_CRITICAL = 0, CLASS_EARLY, CLASS_DEFERRED, CLASS_OPTIONAL, CLASS_COUNT } ImportClass;
static const char *const class_names[CLASS_COUNT] = {
    "boot-critical (linked-module init)",
    "boot-path / likely early (kernel/posix)",
    "deferred subsystem (graphics/audio/input/network/VR)",
    "optional / load-on-demand (PSN/DLC/save/user)"
};
static const char *library_of(const char *name) {
    const char *hash = strchr(name, '#');
    return hash ? hash + 1 : name;
}
/* libScePosix exposes both the low-level file/thread primitives and the socket surface; the sockets are the network
 * subsystem, not the boot path. */
static int is_network_symbol(const char *symbol) {
    static const char *const names[] = {
        "socket", "bind", "listen", "accept", "connect", "send", "recv", "sendto", "recvfrom", "sendmsg", "recvmsg",
        "getsockopt", "setsockopt", "getsockname", "getpeername", "shutdown", "select", "poll", "inet_ntop", "inet_pton",
        "epoll_create", "epoll_ctl", "epoll_wait"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        if (!strcmp(symbol, names[i])) return 1;
    return 0;
}
static ImportClass classify_import(const char *name, const char *symbol, int linked_init) {
    const char *lib = library_of(name);
    if (linked_init) return CLASS_CRITICAL;
    if (!strcmp(lib, "libkernel_unity") || !strcmp(lib, "libkernel_cpumode_platform") ||
        !strcmp(lib, "libSceSysmodule") || !strcmp(lib, "libSceDiscMap") ||
        !strcmp(lib, "libSceCommonDialog") || !strcmp(lib, "libSceRazorCpu") || !strcmp(lib, "ulobjmgr"))
        return CLASS_CRITICAL;
    if (!strcmp(lib, "libkernel") || !strcmp(lib, "libScePosix") || !strcmp(lib, "libc") ||
        !strncmp(lib, "libSceLibcInternal", 18))
        return is_network_symbol(symbol) ? CLASS_DEFERRED : CLASS_EARLY;
    if (!strcmp(lib, "libSceGnmDriver") || !strcmp(lib, "libSceVideoOut") ||
        !strncmp(lib, "libSceAjm", 9) || !strncmp(lib, "libSceAudio", 11) || !strcmp(lib, "libSceAvPlayer") ||
        !strcmp(lib, "libScePad") || !strcmp(lib, "libSceMove") || !strcmp(lib, "libSceMouse") ||
        !strncmp(lib, "libSceIme", 9) || !strcmp(lib, "libSceMsgDialog") || !strcmp(lib, "libSceWebBrowserDialog") ||
        !strncmp(lib, "libSceHmd", 9) || !strcmp(lib, "libSceVrTracker") || !strcmp(lib, "libSceCamera") ||
        !strncmp(lib, "libSceSocialScreen", 18) || !strcmp(lib, "libSceRemoteplay") ||
        !strcmp(lib, "libSceNet") || !strcmp(lib, "libSceHttp") || !strcmp(lib, "libSceSsl") ||
        !strcmp(lib, "libSceNpWebApi") || !strcmp(lib, "libSceNetCtl"))
        return CLASS_DEFERRED;
    return CLASS_OPTIONAL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    sos_ignore_sigpipe();
#endif
#ifdef _WIN32
    { SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize; }
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
#endif
    int check_imports = 0, cpu_only = 0, list_all = 0, skip_launcher = 0, force_launcher = 0, game_child = 0;
    const char *boot = NULL, *app0 = NULL, *content_profile = NULL;
    const char *prepare_view = NULL, *prepare_image = NULL;
    const char *user_dir = "user", *logs_dir = "logs", *log_file = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--list-all")) list_all = 1;
        else if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--sos-skip-launcher")) skip_launcher = 1;
        else if (!strcmp(argv[i], "--launcher")) force_launcher = 1;
        else if (!strcmp(argv[i], "--sos-game")) game_child = 1;
        else if (!strcmp(argv[i], "--app0") && i + 1 < argc) app0 = argv[++i];
        else if (!strcmp(argv[i], "--content-profile") && i + 1 < argc) content_profile = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user_dir = argv[++i];
        else if (!strcmp(argv[i], "--logs") && i + 1 < argc) logs_dir = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_file = argv[++i];
        else if (!strcmp(argv[i], "--prepare-image") && i + 2 < argc) {
            prepare_view = argv[++i];
            prepare_image = argv[++i];
        }
        else if (argv[i][0] != '-' && !boot) boot = argv[i];
        else {
            fprintf(stderr, "Unknown option: %s\nUsage: %s [boot.bin] [--check-imports] [--list-all] [--cpu-only] [--app0 dir] [--content-profile file] [--user dir] [--logs dir] [--log file] [--prepare-image view out] [--sos-skip-launcher] [--launcher]\n", argv[i], argv[0]);
            return 1;
        }
    }

    char log_path[4200];
    /* An existing --log file is continued: the session log tools\build.ps1 -Run started. */
    int log_started = 0;
    if (log_file) log_started = runtime_log_continue(log_file, log_path, sizeof(log_path));
    if (!log_started && !runtime_log_start(logs_dir, log_file, log_path, sizeof(log_path)))
        fprintf(stderr, "Cannot create the log under %s\n", logs_dir);
    printf("[run] God of War: Sons of Sparta PC\n[run] boot %s\n[run] log %s\n", boot ? boot : "(selected by the launcher)", log_path);
    if (app0) printf("[run] app0 %s\n[run] user %s\n", app0, user_dir);
    atexit(runtime_log_flush);
    runtime_log_install_cleanup();

    /* The build tools rebuild the installed game's image from its prepared tree; the launcher owns the installation. */
    if (prepare_view) {
        char error[2048];
        if (sos_build_boot_image(prepare_view, prepare_image, error, sizeof(error))) {
            fprintf(stderr, "ERROR: %s\n", error);
            runtime_log_flush();
            return 1;
        }
        printf("Boot image written to %s\n", prepare_image);
        runtime_log_mark_clean_exit();
        runtime_log_flush();
        return 0;
    }

#ifdef _WIN32
    static const SosCrashHooks crash_hooks = {crash_detail, crash_describe, NULL};
    sos_crash_guard_install(&crash_hooks);
    AddVectoredExceptionHandler(1, vectored_fault);
    signal(SIGABRT, abort_signal);
    _set_invalid_parameter_handler(invalid_parameter);
    _set_purecall_handler(purecall);
#endif

    log_cpu();
    runtime_memory_reserve();
#ifdef _WIN32
    runtime_thread_tcb_offset();
#endif

    SosPaths paths;
    const SosPathRequest path_request = { boot, content_profile, app0, user_dir, logs_dir };
    if (!sos_resolve_paths(&path_request, &paths)) fail(paths.error);
    static char settings_file[4200];
    snprintf(settings_file, sizeof(settings_file), "%s/sons_of_sparta.ini", paths.user_dir);
    runtime_log_set_detailed(sos_detailed_logging(settings_file));
    if (game_child) {
        puts("Startup: game-only child process");
        sos_startup_inherit_play();
        sos_startup_milestone("game process running");
    } else if (!check_imports && !cpu_only) {
        int game_process = 0;
#ifdef _WIN32
        game_process = 1;
#endif
        static char log_folder[4200];
        if (!sos_logs_directory(paths.logs_dir, log_started ? log_path : NULL, log_folder, sizeof(log_folder)))
            snprintf(log_folder, sizeof(log_folder), "%s", paths.logs_dir);
        const SosLauncherConfig launcher = { paths.app0, paths.image_dir, paths.user_dir, settings_file, log_folder,
                                            skip_launcher, force_launcher, NULL, game_process,
                                            log_started ? log_path : NULL };
        sos_startup_phase("launcher handoff");
        const SosLauncherResult launched = sos_launcher_run(&launcher);
        sos_startup_phase("launcher returned");
        if (launched == SOS_LAUNCHER_PLAY) sos_startup_milestone("launcher closed");
        runtime_log_set_detailed(sos_detailed_logging(settings_file));
        if (launched == SOS_LAUNCHER_QUIT) {
            runtime_log_mark_clean_exit();
            runtime_log_flush();
            return 0;
        }
        if (launched == SOS_LAUNCHER_BLOCKED) return 1;
#ifdef _WIN32
        if (launched == SOS_LAUNCHER_GAME_STARTED) {
            /* The game process appends to this log: nothing here may write to it once it runs. */
            puts("Launcher: the game process continues this log");
            runtime_log_flush();
            runtime_log_release();
            if (!sos_resume_game_process()) {
                runtime_log_write_direct("Launcher: could not start the game process\n");
                return 1;
            }
            return 0;
        }
#endif
        if (!sos_resolve_paths(&path_request, &paths)) fail(paths.error);
    }
    printf("Overlay compatibility: %s game process\n", game_child ? "separate" : "original");
    boot = paths.boot;
    content_profile = paths.content_profile;
    app0 = paths.app0;
    user_dir = paths.user_dir;
    logs_dir = paths.logs_dir;
    if (!boot || !boot[0]) fail("no game is installed; install it from the launcher");
    load_content_profile(content_profile);
    report_image(boot);
    load_module_names(boot);
    sos_log_settings_summary(settings_file, paths.image_dir);

    static SosControllerIcons controller_icons;
    {
        SosGameBuild icons_build;
        int icons_supported = 0;
        if (sos_game_build(paths.image_dir, &icons_build)) {
            char icons_key[64];
            snprintf(icons_key, sizeof(icons_key), "%s-%s", icons_build.title_id, icons_build.version);
            icons_supported = sos_controller_prompts_supported(icons_key);
        }
        if (!check_imports && !cpu_only)
            sos_prepare_controller_icons(app0, paths.image_dir, sos_controller_prompts_from_file(settings_file), icons_supported,
                                         &controller_icons);
        for (const char *line = controller_icons.report; *line;) {
            const char *end = strchr(line, '\n');
            const int length = end ? (int)(end - line) : (int)strlen(line);
            if (length) printf("Controller icons: %.*s\n", length, line);
            line += length + (end ? 1 : 0);
        }
    }

    {
        if (app0) runtime_file_configure(app0, user_dir);
        if (controller_icons.assets[0] && controller_icons.streamed[0] &&
            runtime_file_alias("/app0/Media/resources.assets", controller_icons.assets) == 0 &&
            runtime_file_alias("/app0/Media/resources.assets.resS", controller_icons.streamed) == 0)
            puts("Controller icons: prepared copies in use");
        runtime_savedata_configure("CUSA57767");
        trophy_runtime_configure(app0, user_dir);
        runtime_pad_set_settings(settings_file);
    }
    dcpgpu_register_kernel();

    FILE *f = fopen(boot, "rb");
    if (!f) fail("cannot open boot file");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SOSBOOT1", 8)) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    image_bytes = size;
    import_count = read64(f);
    uint64_t capabilities = read64(f);
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    sos_startup_phase("runtime initialization");
    runtime_reset_process_clock();
    runtime_start(capabilities);
    if (!size || size > 512 * 1024 * 1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int native_libc = (capabilities & 1);
    uint64_t main_tls[4] = {0};
    uint64_t nb = 0, procparam = 0;
    uint64_t *bindings = calloc(import_count ? import_count : 1, sizeof(*bindings));
    uint64_t *binding_kinds = calloc(import_count ? import_count : 1, sizeof(*binding_kinds));
    import_resolved = calloc(import_count ? import_count : 1, 1);
    if (!bindings || !binding_kinds || !import_resolved) fail("allocation failed");
    procparam = read64(f);
    for (int i = 0; i < 4; ++i) main_tls[i] = read64(f);
    module_count = read64(f);
    if (!module_count || module_count > sizeof(modules) / sizeof(*modules)) fail("invalid module count");
    for (uint64_t m = 0; m < module_count; ++m) {
        LinkedModule *x = &modules[m];
        x->base = read64(f); x->size = read64(f); x->init = read64(f); x->tls_address = read64(f);
        x->tls_memsz = read64(f); x->tls_filesz = read64(f); x->tls_module = read64(f);
    }
    nb = read64(f);
    if (main_tls[1] > main_tls[2] || main_tls[2] > 1024 * 1024 || main_tls[0] > size ||
        main_tls[1] > size - main_tls[0] || (main_tls[3] & (main_tls[3] - 1)) || main_tls[3] > 4096)
        fail("invalid eboot TLS metadata");
    for (uint64_t m = 0; m < module_count; ++m) {
        LinkedModule *x = &modules[m];
        if (x->base >= size || !x->size || x->size > size - x->base || x->init < x->base || x->init - x->base >= x->size ||
            (x->tls_module && (x->tls_address >= size || x->tls_memsz > size - x->tls_address || x->tls_memsz > 1024 * 1024 ||
                               x->tls_filesz > x->tls_memsz || x->tls_module < 2 || x->tls_module > 7)))
            fail("invalid linked module metadata");
    }
    if (nb > import_count) fail("invalid binding count");
    for (uint64_t i = 0; i < nb; ++i) {
        uint64_t index = read64(f), address = read64(f), kind = read64(f);
        int inside = 0;
        for (uint64_t m = 0; m < module_count; ++m)
            if (address >= modules[m].base && address - modules[m].base < modules[m].size) inside = 1;
        if (index >= import_count || !inside || (kind != 1 && kind != 2) || bindings[index]) fail("invalid native binding");
        bindings[index] = address; binding_kinds[index] = kind;
        import_resolved[index] = 1;
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    /* The table is four little-endian u64 per entry, as Reloc is on x86-64: one read, every entry still checked. */
    _Static_assert(sizeof(Reloc) == 32, "boot file relocation layout");
    if (fread(relocs, sizeof(*relocs), nr, f) != nr) fail("truncated boot file");
    for (uint64_t i = 0; i < nr; ++i) {
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m = 0; m < module_count; ++m)
        if (!mapped(segments, ns, modules[m].init, 1) || (modules[m].tls_module && !mapped(segments, ns, modules[m].tls_address, modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (!mapped(segments, ns, procparam, 64)) fail("unmapped procparam");
    for (uint64_t i = 0; i < import_count; ++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments, ns, bindings[i], 1)) fail("unmapped native export");
        if (binding_kinds[i] == 1) {
            int executable = 0;
            for (uint64_t s = 0; s < ns; ++s)
                if ((segments[s].flags & 1) && bindings[i] >= segments[s].address &&
                    bindings[i] - segments[s].address < segments[s].size) executable = 1;
            if (!executable) fail("native function is not executable");
        }
    }

    const int skip_intro_wanted = sos_skip_intro_from_file(settings_file) && sos_skip_intro_supported(paths.image_dir);
    const size_t stub_area = round_page(SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES + SOS_PROMPT_DIALOG_STUB_BYTES);
    image = allocate(round_page(size) + stub_area);
    runtime_set_guest_image(image,size);
#ifdef _WIN32
    image_size = size;
    sos_crash_guard_set_guest((uintptr_t)image, size);
#endif
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    sos_startup_milestone("guest image read, %" PRIu64 " relocations checked", nr);
    SosRenderTarget render_target = sos_native_render_target();
    {
        SosGameBuild game_build;
        const int known_build = sos_game_build(paths.image_dir, &game_build);
        const int supported = known_build && game_build.resolution_site_count != 0;
        int resolution_choice = 0, aspect_choice = 0;
        const SosRenderTarget requested =
            sos_render_target_from_settings(settings_file, supported, &resolution_choice, &aspect_choice);
        if (!sos_render_target_is_native(&requested)) {
            char error[320];
            if (sos_apply_render_target(image, size, game_build.resolution_sites, game_build.resolution_site_count,
                                        &requested, error, sizeof(error))) {
                render_target = requested;
                note_render_target_values(&game_build, &requested);
                printf("Patch: render target %dx%d, Pro mode %s, direct memory pool +%llu MiB\n", render_target.width,
                       render_target.height, render_target.neo ? "on" : "off",
                       (unsigned long long)(sos_budget_growth(&render_target) >> 20));
            } else {
                printf("Display: render resolution ignored: %s, game build %s\n", error, game_build.label);
                resolution_choice = aspect_choice = 0;
            }
        }
        runtime_sos_set_neo_mode(render_target.neo);
        runtime_memory_set_direct_extra(sos_budget_growth(&render_target));
        sos_crash_context_set_display(render_target.width, render_target.height, sos_resolutions[resolution_choice].key,
                                      sos_aspect_ratios[aspect_choice].key);
        printf("Display: render resolution %dx%d (%s), aspect ratio %s\n", render_target.width, render_target.height,
               sos_resolutions[resolution_choice].key, sos_aspect_ratios[aspect_choice].name);
    }
    const int prompt_style = sos_controller_prompts_from_file(settings_file);
    char prompt_key[64] = "";
    {
        SosGameBuild game_build;
        const int known_build = sos_game_build(paths.image_dir, &game_build);
        if (known_build) {
            snprintf(prompt_key, sizeof(prompt_key), "%s-%s", game_build.title_id, game_build.version);
            uint64_t offsets[16];
            uint8_t lengths[16];
            const size_t values = sos_controller_prompts_value_ranges(image, size, prompt_key, offsets, lengths, 16);
            for (size_t i = 0; i < values; ++i) runtime_redzone_add_volatile(offsets[i], lengths[i]);
        }
        if (prompt_style == SOS_PROMPTS_XBOX && known_build) {
            char message[200];
            sos_controller_prompts_apply(image, size, prompt_key, prompt_style, message, sizeof(message));
            printf("Controller prompts: %s\n", message);
        } else if (prompt_style == SOS_PROMPTS_XBOX) {
            printf("Controller prompts: Xbox requested but unavailable: not a supported game build\n");
        } else if (prompt_style == SOS_PROMPTS_AUTOMATIC && !known_build) {
            printf("Controller prompts: Automatic requested but unavailable: not a supported game build\n");
        }
    }
    if (runtime_memory_prepare()) fail("guest memory unavailable");
    {
        uint64_t sdk = 0;
        memcpy(&sdk, image + procparam + 16, 8);
        const uint32_t gpu_features = sos_skip_intro_supported(paths.image_dir) ? DCP_GPU_FEATURE_SKIP_INTRO : 0;
        DcpGpuConfig gpu = { "God of War: Sons of Sparta", "CUSA57767", user_dir, settings_file,
                             (uint32_t)sdk, 0, 1920, 1080, gpu_features, render_target.display_width,
                             render_target.display_height };
        if (dcpgpu_available() && dcpgpu_init(&gpu) == 0) {
#ifdef _WIN32
            gpu_enabled = 1;
#endif
            printf("GPU: window and presenter ready, %u HLE symbols\n", dcpgpu_symbol_count());
            sos_startup_milestone("GPU ready");
        } else
            printf("GPU: video core not started (see the log)\n");
    }

    printf("Modules: %" PRIu64 "\n", module_count);
    if (runtime_module_register_main("eboot.bin", image, module_count ? modules[0].base : size) != 0)
        fail("cannot register the main executable");
    for (uint64_t m = 0; m < module_count; ++m) {
        const char *path = module_path(m);
        if (!path) fail("linked module path missing from image.json");
        if (runtime_module_register_linked(path, image + modules[m].base, modules[m].size, modules[m].base) < 0)
            fail("cannot register linked module");
        printf("  module %" PRIu64 " %s base 0x%" PRIx64 " size 0x%" PRIx64 " init 0x%" PRIx64 "\n",
               m, path, modules[m].base, modules[m].size, modules[m].init);
    }

    unsigned char *traps = allocate(round_page((import_count + 1) * 64));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 64;
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        size_t at = 0;
        /* Keep the guest return address at [rsp] and six argument registers at [rsp+8..]. */
        t[at++] = 0x48; t[at++] = 0x8b; t[at++] = 0x04; t[at++] = 0x24; /* mov rax,[rsp] */
        t[at++] = 0x48; t[at++] = 0x83; t[at++] = 0xec; t[at++] = 0x40; /* sub rsp,0x40 */
        t[at++] = 0x48; t[at++] = 0x89; t[at++] = 0x04; t[at++] = 0x24; /* mov [rsp],rax */
        t[at++] = 0x48; t[at++] = 0x89; t[at++] = 0x7c; t[at++] = 0x24; t[at++] = 0x08;
        t[at++] = 0x48; t[at++] = 0x89; t[at++] = 0x74; t[at++] = 0x24; t[at++] = 0x10;
        t[at++] = 0x48; t[at++] = 0x89; t[at++] = 0x54; t[at++] = 0x24; t[at++] = 0x18;
        t[at++] = 0x48; t[at++] = 0x89; t[at++] = 0x4c; t[at++] = 0x24; t[at++] = 0x20;
        t[at++] = 0x4c; t[at++] = 0x89; t[at++] = 0x44; t[at++] = 0x24; t[at++] = 0x28;
        t[at++] = 0x4c; t[at++] = 0x89; t[at++] = 0x4c; t[at++] = 0x24; t[at++] = 0x30;
        t[at++] = 0xbf; memcpy(t + at, &index, 4); at += 4;
        t[at++] = 0x48; t[at++] = 0x8d; t[at++] = 0x74; t[at++] = 0x24; t[at++] = 0x08; /* lea rsi,[rsp+8] */
        t[at++] = 0x48; t[at++] = 0xb8; memcpy(t + at, &handler, 8); at += 8;
        t[at++] = 0xff; t[at++] = 0xe0;
    }
    /* One lookup per import and kind: an import resolves the same way for every relocation that names it. */
    uintptr_t *resolved_imports = calloc(import_count ? 2 * import_count : 1, sizeof(*resolved_imports));
    unsigned char *looked_up = calloc(import_count ? 2 * import_count : 1, 1);
    if (!resolved_imports || !looked_up) fail("allocation failed");
    uint64_t lookups = 0;
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 64 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            const uint64_t slot = 2 * relocs[i].value + (relocs[i].kind == 2);
            if (!looked_up[slot]) {
                resolved_imports[slot] = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
                looked_up[slot] = 1;
                ++lookups;
            }
            uintptr_t resolved = resolved_imports[slot];
            if (resolved) { value = resolved + relocs[i].addend; import_resolved[relocs[i].value] = 1; }
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address = bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value] != relocs[i].kind || !mapped(segments, ns, address, relocs[i].addend + 1))
                    fail("native export kind/range mismatch");
                value = (uintptr_t)image + address + relocs[i].addend;
                import_resolved[relocs[i].value] = 1;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    free(resolved_imports);
    free(looked_up);
    const uint64_t stubs = patch_import_stubs(relocs, nr);
    if (stubs) printf("Patched %" PRIu64 " direct import stubs to their GOT entries\n", stubs);
    sos_startup_phase("imports resolved");
    sos_startup_milestone("relocations applied, %" PRIu64 " import lookups", lookups);
    for (uint64_t s = 0; s < ns; ++s) {
        if (!(segments[s].flags & 1)) continue;
        for (uint64_t i = 0; i < nr; ++i)
            if (relocs[i].target >= segments[s].address && relocs[i].target - segments[s].address < segments[s].size)
                runtime_redzone_add_volatile(relocs[i].target, 8);
    }
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);

    if (check_imports) {
        unsigned char *checked = calloc(import_count ? import_count : 1, 1);
        unsigned char *import_class = malloc(import_count ? import_count : 1);
        unsigned short *importers = calloc(import_count ? import_count : 1, sizeof(unsigned short));
        if (!checked || !import_class || !importers) fail("allocation failed");
        memset(import_class, 0xFF, import_count ? import_count : 1);
        const uint64_t eboot_boundary = module_count ? modules[0].base : size;
        for (uint64_t i = 0; i < nr; ++i) {
            if (!relocs[i].kind) continue;
            unsigned bit = 0;
            if (relocs[i].target < eboot_boundary) bit = 1u;
            else for (uint64_t m = 0; m < module_count; ++m)
                if (relocs[i].target >= modules[m].base && relocs[i].target - modules[m].base < modules[m].size) { bit = 1u << (1 + m); break; }
            if (bit) importers[relocs[i].value] |= (unsigned short)bit;
        }
        uint64_t counts[CLASS_COUNT] = {0};
        uint64_t resolved_count = 0, total = 0;
        for (uint64_t i = 0; i < nr; ++i) {
            uint64_t index = relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index] = 1;
            ++total;
            if (runtime_resolve(names[index], relocs[i].kind == 2) || (native_libc && bindings[index])) { ++resolved_count; continue; }
            int linked_init = (importers[index] & 0x6) != 0;
            import_class[index] = (unsigned char)classify_import(names[index], runtime_import_name(names[index]), linked_init);
            ++counts[import_class[index]];
        }
        uint64_t missing = counts[CLASS_CRITICAL] + counts[CLASS_EARLY] + counts[CLASS_DEFERRED] + counts[CLASS_OPTIONAL];
        printf("Imports: %" PRIu64 " resolved, %" PRIu64 " missing of %" PRIu64 "\n", resolved_count, missing, total);
        for (int c = 0; c < CLASS_COUNT; ++c)
            printf("  %-56s %" PRIu64 "\n", class_names[c], counts[c]);
        printf("\n");
        /* Normal run lists the actionable boot-critical and early lines; -CheckImports lists every class. */
        int last = list_all ? CLASS_OPTIONAL : CLASS_EARLY;
        for (int c = CLASS_CRITICAL; c <= last; ++c) {
            if (!counts[c]) continue;
            printf("MISSING (%s):\n", class_names[c]);
            for (uint64_t i = 0; i < import_count; ++i)
                if (import_class[i] == (unsigned char)c)
                    printf("  %s (%s)\n", names[i], runtime_import_name(names[i]));
            printf("\n");
        }
        if (!list_all) printf("Full unresolved inventory: rerun with -CheckImports.\n");
        printf("Exit: %s\n", missing ? "missing imports" : "all imports resolved");
        runtime_log_flush();
        _exit(missing ? 1 : 0);
    }
    (void)cpu_only;

#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
    if (gpu_enabled) {
        uint64_t *triples = calloc(ns * 3, sizeof(*triples));
        if (!triples) fail("allocation failed");
        for (uint64_t i = 0; i < ns; ++i) {
            triples[3 * i] = segments[i].address;
            triples[3 * i + 1] = segments[i].size;
            triples[3 * i + 2] = segments[i].flags;
        }
        char redzone_cache[4200];
        snprintf(redzone_cache, sizeof(redzone_cache), "%s/cache/CUSA57767/redzone", user_dir);
        runtime_redzone_set_cache(redzone_cache);
        runtime_redzone_install(image, size, triples, ns);
        free(triples);
        sos_startup_milestone("red zone ready");
    }
#endif
    if (skip_intro_wanted) {
        SosGameBuild skip_build;
        if (sos_game_build(paths.image_dir, &skip_build)) {
            char skip_key[64];
            snprintf(skip_key, sizeof(skip_key), "%s-%s", skip_build.title_id, skip_build.version);
            char skip_message[160];
            sos_skip_intro_apply(image, size, skip_key, image + round_page(size), stub_area, skip_message, sizeof(skip_message));
            printf("Skip Intro: %s\n", skip_message);
        }
    }
    if (prompt_style == SOS_PROMPTS_AUTOMATIC && prompt_key[0]) {
        char hook_message[200];
        sos_prompt_hook_arm(prompt_style, prompt_key);
        if (sos_prompt_hook_install(image, size, prompt_key, image + round_page(size) + SOS_SKIP_INTRO_STUB_BYTES,
                                    SOS_PROMPT_HOOK_STUB_BYTES, hook_message, sizeof(hook_message)))
            printf("Controller prompts: Automatic (%s)\n", hook_message);
        else
            printf("Controller prompts: Automatic requested but unavailable: %s; the game's own prompts are kept\n", hook_message);
    } else if (prompt_style != SOS_PROMPTS_XBOX) {
        printf("Controller prompts: %s\n", sos_prompt_style_label(prompt_style));
    }
    if (prompt_style != SOS_PROMPTS_PLAYSTATION && prompt_key[0]) {
        char dialog_message[200];
        if (sos_prompt_dialog_install(image, size, prompt_key,
                                      image + round_page(size) + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES,
                                      SOS_PROMPT_DIALOG_STUB_BYTES, dialog_message, sizeof(dialog_message))) {
            sos_prompt_dialog_arm(prompt_style, image, prompt_key);
            printf("Controller prompts: %s\n", dialog_message);
        } else {
            printf("Controller prompts: Settings dialogs keep the PlayStation glyphs: %s\n", dialog_message);
        }
    }
    protect(traps, round_page((import_count + 1) * 64), 5);
    protect(image, round_page(size), 0);
    protect(image + round_page(size), stub_area, 5);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    sos_startup_phase("guest image mapped");
    if (native_libc) {
        for (uint64_t m = 0; m < module_count; ++m) {
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module, image + modules[m].tls_address,
                                       modules[m].tls_filesz, modules[m].tls_memsz);
        }
        runtime_set_main_tls(image + main_tls[0], main_tls[1], main_tls[2], main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image + procparam);
        sos_startup_phase("linked modules initialization");
        for (uint64_t m = 0; m < module_count; ++m) {
#ifdef _WIN32
            current_module = (int)m;
#endif
            printf("Starting linked module %" PRIu64 " %s entry 0x%" PRIx64 " (image+0x%" PRIx64 ")\n", m,
                   module_label(m), (uint64_t)(uintptr_t)(image + modules[m].init), modules[m].init);
            {
                char event[96];
                snprintf(event,sizeof(event),"linked module %llu %s",(unsigned long long)m,module_label(m));
                runtime_history_note(event,"loader",0);
            }
            typedef int (ABI *ModuleInit)(uint64_t, void *, void *);
            int result = ((ModuleInit)(image + modules[m].init))(0, NULL, NULL);
            runtime_module_linked_started(modules[m].init, result);
            printf("Module %" PRIu64 " %s initializer returned %d\n", m, module_label(m), result);
#ifdef _WIN32
            current_module = -1;
#endif
            if (result) fail("module initializer failed");
        }
        sos_startup_milestone("linked modules initialized");
    }
    sos_startup_phase("guest entry");
    start_stall_watchdog();
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    printf("Runtime: Unity bootstrap entered\n");
    sos_startup_milestone("Unity bootstrap entered");
    sos_checkpoint("Unity bootstrap entered at guest image+0x%" PRIx64,entry);
    sos_startup_profile_begin(image, image_size);
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    enum { MAIN_STACK = 8 * 1024 * 1024 };
    unsigned char *stack = runtime_low_map(MAIN_STACK, PROT_READ | PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image + entry, &params, (void *)guest_exit, stack + MAIN_STACK - 64);
#endif
    fprintf(stderr,"Runtime: guest entry unexpectedly returned; entry=guest image+0x%" PRIx64 ", startup phase=%s, module operation=%s\n",
            entry,sos_startup_phase_current(),runtime_module_operation());
    char graphics[256];
    dcpgpu_first_frame_summary(graphics, sizeof(graphics));
    fprintf(stderr,"Runtime: graphics progress: %s\n",graphics);
    runtime_history_dump();
    runtime_log_flush();
    runtime_exit(1);
}
