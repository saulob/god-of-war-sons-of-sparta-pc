/* The Windows module (a DLL or the executable) that holds an address, read from its headers in memory: no loader lock,
 * no heap and no file access, so the crash report can use it on any thread. Only the module's own name is kept, never a
 * path. */
#ifndef SOS_HOST_MODULE_H
#define SOS_HOST_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t base;
    uint32_t image_size;
    uint32_t timestamp;
    uint16_t version[4];
    int has_version;
    char name[64];
} SosHostModule;

/* Copies `size` bytes at `address` into `out`; 1 when all of them could be read. */
typedef int (*SosHostModuleRead)(void *context, uintptr_t address, void *out, size_t size);

/* The 64-bit PE image starting at `base`; 0 when its headers are not one. */
int sos_host_module_parse(uintptr_t base, SosHostModuleRead read, void *context, SosHostModule *out);
/* The mapped image of this process that holds `address`; 0 for any other memory. */
int sos_host_module_at(uintptr_t address, SosHostModule *out);
/* "module=<name> module_base=0x... module_offset=0x... module_size=0x... file_version=a.b.c.d timestamp=0x...";
 * always NUL-terminated, returns the length. */
size_t sos_host_module_format(const SosHostModule *module, uintptr_t address, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
