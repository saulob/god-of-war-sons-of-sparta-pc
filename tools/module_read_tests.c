/* Headless test of the module loader's file read (runtime_module.c read_file): exact contents and sizes for empty,
 * small, unseekable and short-read files, and (with a path) the time to read a real module. No game is started. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../src/runtime_module.c"

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
void runtime_describe_guest_address(uintptr_t address, char *out, size_t size) { (void)address; snprintf(out, size, "test"); }
uint64_t runtime_add_module_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align) { (void)data; (void)filesz; (void)memsz; (void)align; return 0; }
void runtime_remove_module_tls(uint64_t module) { (void)module; }
uint32_t runtime_thread_tcb_offset(void) { return 0; }
void runtime_history_note(const char *api, const char *library, uintptr_t caller) { (void)api; (void)library; (void)caller; }
uintptr_t runtime_resolve(const char *name, int is_data) { (void)name; (void)is_data; return 0; }
void runtime_symbol_to_nid(const char *symbol, char *out) { (void)symbol; out[0] = 0; }
const char *runtime_import_name(const char *name) { return name; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

/* fd 7: an in-memory file; seekable or not, and reads of at most `chunk` bytes. fd 8: a real host file. */
static unsigned char *memory_file;
static size_t memory_size, memory_position, chunk;
static int seekable, opens, closes, reads;
static FILE *host_file;
int64_t runtime_file_open(const char *path, int flags, int mode) {
    (void)flags; (void)mode;
    ++opens;
    if (!strcmp(path, "/memory")) { memory_position = 0; return 7; }
    host_file = fopen(path, "rb");
    return host_file ? 8 : -2;
}
int64_t runtime_file_close(int fd) {
    ++closes;
    if (fd == 8 && host_file) { fclose(host_file); host_file = NULL; }
    return 0;
}
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) {
    ++reads;
    if (fd == 8) return (int64_t)fread(buffer, 1, (size_t)size, host_file);
    size_t n = memory_size - memory_position;
    if (n > size) n = (size_t)size;
    if (chunk && n > chunk) n = chunk;
    memcpy(buffer, memory_file + memory_position, n);
    memory_position += n;
    return (int64_t)n;
}
int64_t runtime_file_lseek(int fd, int64_t offset, int whence) {
    if (fd == 8) {
        if (_fseeki64(host_file, offset, whence)) return -22;
        return _ftelli64(host_file);
    }
    if (!seekable) return -29;
    memory_position = whence == 2 ? memory_size + (size_t)offset : (size_t)offset;
    return (int64_t)memory_position;
}

static int failures;
static void check(const char *name, int condition) {
    printf(condition ? "  [ok]   %s\n" : "  [fail] %s\n", name);
    if (!condition) ++failures;
}
static void case_read(const char *name, size_t size, int can_seek, size_t chunk_size, int max_reads) {
    memory_file = malloc(size ? size : 1);
    for (size_t i = 0; i < size; ++i) memory_file[i] = (unsigned char)(i * 131 + 7);
    memory_size = size; seekable = can_seek; chunk = chunk_size; reads = 0;
    const int closes_before = closes;
    uint64_t got = 12345;
    unsigned char *data = (unsigned char *)read_file("/memory", &got);
    char what[160];
    snprintf(what, sizeof(what), "%s: every byte, in order, and the size", name);
    check(what, data && got == size && (!size || !memcmp(data, memory_file, size)));
    snprintf(what, sizeof(what), "%s: at most %d reads (%d)", name, max_reads, reads);
    check(what, reads <= max_reads);
    snprintf(what, sizeof(what), "%s: the file is closed", name);
    check(what, closes == closes_before + 1);
    free(data);
    free(memory_file);
}

int main(int argc, char **argv) {
    case_read("empty file", 0, 1, 0, 1);
    case_read("small file", 1000, 1, 0, 2);
    case_read("4 MiB file", 4u << 20, 1, 0, 2);
    case_read("4 MiB file, 64 KiB reads", 4u << 20, 1, 65536, 66);
    case_read("unseekable 5 MiB file", 5u << 20, 0, 0, 5);
    case_read("unseekable 5 MiB file, 64 KiB reads", 5u << 20, 0, 65536, 82);
    uint64_t size = 0;
    check("a missing file fails", !read_file("/missing", &size));
    if (argc > 1) {
        LARGE_INTEGER t0, t1, hz;
        QueryPerformanceFrequency(&hz);
        QueryPerformanceCounter(&t0);
        unsigned char *data = (unsigned char *)read_file(argv[1], &size);
        QueryPerformanceCounter(&t1);
        const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
        printf("  %s: %llu bytes in %.1f ms\n", argv[1], (unsigned long long)size, ms);
        check("the real module is read whole", data && size > (100u << 20));
        check("the real module reads in under 1 s", ms < 1000.0);
        free(data);
    }
    printf("%s\n", failures ? "module read tests FAILED" : "all module read tests passed");
    return failures != 0;
}
