/* Headless test of the module loader's CPU compatibility (runtime_module.c plan_module_compat, install_module_compat and
 * protect_cpu_compat, as load_new_module calls them) on the real code of libfmod.prx and libfmodstudio.prx exported by
 * tools\export_code_ranges.py, for every processor profile and with everything forced. No game is started.
 *   cpu_compat_module_tests NAME DIR ... */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../src/runtime_module.c"
#include "cpu_compat_profiles.h"

int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
FILE *sos_stderr_stream(void) { return stdout; }
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) { (void)api; (void)status; (void)caller; }
void runtime_log_flush(void) {}
const char *sos_startup_phase_current(void) { return "test"; }
void runtime_describe_guest_address(uintptr_t address, char *out, size_t size) { (void)address; snprintf(out, size, "test"); }
uint64_t runtime_add_module_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align) { (void)data; (void)filesz; (void)memsz; (void)align; return 0; }
void runtime_remove_module_tls(uint64_t module) { (void)module; }
uint32_t runtime_thread_tcb_offset(void) { return 0; }
uint32_t runtime_thread_thread_offset(void) { return 0; }
void runtime_history_note(const char *api, const char *library, uintptr_t caller) { (void)api; (void)library; (void)caller; }
uintptr_t runtime_resolve(const char *name, int is_data) { (void)name; (void)is_data; return 0; }
void runtime_symbol_to_nid(const char *symbol, char *out) { (void)symbol; out[0] = 0; }
const char *runtime_import_name(const char *name) { return name; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
int64_t runtime_file_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -2; }
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) { (void)fd; (void)buffer; (void)size; return -1; }
int64_t runtime_file_lseek(int fd, int64_t offset, int whence) { (void)fd; (void)offset; (void)whence; return -1; }

static unsigned checks, failures;
static void check(int ok, const char *what, unsigned long long a, unsigned long long b) {
    ++checks;
    if (!ok && ++failures <= 40) printf("  FAIL %s (%#llx, %#llx)\n", what, a, b);
}

static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    if (data) { data[n] = 0; *size = (size_t)n; }
    return data;
}

typedef struct { int installed; size_t replaced; unsigned sites; int intact, stubs_clean; } Loaded;

/* The module as load_new_module maps it on `host`: plan, allocation with the stubs after it, the code, the install
 * and the protection. `change` flips one byte of the code first (0: none); `extra` grows the mapped size. */
static Loaded load(const char *name, const char *path_name, const uint8_t *code, size_t code_size, uint64_t mapped,
                   IsaHostFeatures host, unsigned forced, uint64_t change, uint64_t extra) {
    LoadedModule m;
    memset(&m, 0, sizeof m);
    m.used = 1;
    m.size = mapped + extra;
    snprintf(m.path, sizeof m.path, "/app0/Media/Plugins/%s", path_name);
    const CpuCompatPlan plan = plan_module_compat(&m, host, forced);
    m.base = (uintptr_t)module_alloc(page_round(m.size) + m.compat_bytes);
    uint8_t *base = (uint8_t *)(uintptr_t)m.base, *stubs = base + page_round(m.size);
    memcpy(base, code, code_size);
    if (change) base[change] ^= 0x01;
    uint8_t *expected = malloc((size_t)m.size);
    memcpy(expected, base, (size_t)m.size);
    Loaded result = {install_module_compat(&m, &plan), 0, 0, 0, 1};
    const CpuCompatTable *table = cpu_compat_module_table(name);
    const int written = result.installed && plan.wanted && !change && !extra;
    for (size_t i = 0; i < m.compat_bytes && !written; ++i) result.stubs_clean &= stubs[i] == 0;
    if (written) {
        const unsigned kinds = plan.kinds;
        result.replaced = table_parts(table, kinds);
        result.intact = installed_as_planned(table, kinds, expected, base, table->size, stubs, m.compat_bytes, result.replaced,
                                             &result.sites);
    } else {
        result.intact = !memcmp(base, expected, (size_t)m.size);
    }
    protect_cpu_compat(&m);
    free(expected);
    abandon_module(&m);
    return result;
}

static void test_module(const char *name, const char *directory) {
    char path[1024];
    size_t code_size = 0, size_text = 0;
    snprintf(path, sizeof path, "%s/%s.code", directory, name);
    uint8_t *code = read_all(path, &code_size);
    snprintf(path, sizeof path, "%s/%s.size", directory, name);
    char *mapped_text = (char *)read_all(path, &size_text);
    const CpuCompatTable *table = cpu_compat_module_table(name);
    if (!code || !mapped_text || !table) { check(0, "module exported and known", 0, 0); free(code); free(mapped_text); return; }
    const uint64_t mapped = strtoull(mapped_text, NULL, 10);
    unsigned table_kinds = 0;
    for (size_t i = 0; i < table->count; ++i) table_kinds |= cpu_compat_site_kinds(&table->sites[i]);
    unsigned normal = 0, compatible = 0;
    const size_t count = sizeof profiles / sizeof *profiles;
    for (size_t i = 0; i <= count; ++i) {
        const int forced = i == count;
        const Profile *p = forced ? &profiles[0] : &profiles[i];
        if (!forced && !p->usable) continue;
        const unsigned kinds = forced ? CPU_COMPAT_ALL : p->compat;
        const Loaded l = load(name, name, code, code_size, mapped, p->host, forced ? CPU_COMPAT_ALL : 0, 0, 0);
        const unsigned wanted = kinds & table_kinds;
        check(l.installed && l.intact && l.replaced == table_parts(table, wanted ? kinds : 0), forced ? "forced" : p->name,
              l.replaced, wanted);
        if (!wanted) check(l.stubs_clean && l.sites == 0, p->name, l.sites, 0);
        wanted ? ++compatible : ++normal;
        char fallback[64];
        cpu_compat_kind_names(wanted, fallback, sizeof fallback);
        printf("  %-18s %-30s replaced at load: %-14s %3zu instructions in %3u sites\n", name, forced ? "forced compatibility" : p->name,
               *fallback ? fallback : "none", l.replaced, l.sites);
    }
    const uint64_t last = table->sites[table->count - 1].offset + table->sites[table->count - 1].length - 1;
    const IsaHostFeatures sandy = profiles[2].host, no_bmi1 = profiles[12].host;
    const Loaded changed = load(name, name, code, code_size, mapped, sandy, 0, last, 0);
    check(!changed.installed && changed.intact && changed.stubs_clean, "a changed byte refuses the module", changed.installed, 0);
    const Loaded other_size = load(name, name, code, code_size, mapped, sandy, 0, 0, 4096);
    check(!other_size.installed && other_size.intact && other_size.stubs_clean, "another size refuses the module", other_size.installed, 0);
    const Loaded forced_changed = load(name, name, code, code_size, mapped, profiles[8].host, CPU_COMPAT_ALL, last, 0);
    check(!forced_changed.installed && forced_changed.intact, "a changed byte refuses the forced install", forced_changed.installed, 0);
    /* Without BMI1 alone libfmod's BMI1 sites only avoid a trap: a mismatch leaves them to the #UD emulation. */
    const Loaded fallback = load(name, name, code, code_size, mapped, no_bmi1, 0, last, 0);
    check(fallback.installed == !(profiles[12].compat & table_kinds & (CPU_COMPAT_LZCNT | CPU_COMPAT_TZCNT | CPU_COMPAT_F16C)) &&
          fallback.intact, "only LZCNT, TZCNT and F16C stop a module", fallback.installed, 0);
    const Loaded unknown = load(name, "SaveData.prx", code, code_size, mapped, sandy, 0, 0, 0);
    check(unknown.installed && unknown.intact, "a module without a table is left alone", unknown.installed, 0);
    printf("  %-18s normal path PASS on %u profiles, compatibility path PASS on %u (forced included); refused with nothing "
           "written: one changed byte, another size; a module without a table left alone\n", name, normal, compatible);
    free(code);
    free(mapped_text);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i + 1 < argc; i += 2) {
        printf("Module loader on %s\n", argv[i]);
        test_module(argv[i], argv[i + 1]);
    }
    printf("%u checks, %u failed\n", checks, failures);
    return failures || argc < 3 ? 1 : 0;
}
