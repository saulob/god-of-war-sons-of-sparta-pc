#include "host_module.h"

#include <windows.h>

#define VERSION_SIGNATURE 0xFEEF04BDu
#define MAX_RESOURCE_ENTRIES 256u
#define VERSION_SCAN_BYTES 256u

typedef struct {
    uintptr_t base;
    uint32_t image_size;
    SosHostModuleRead read;
    void *context;
} Image;

static int read_rva(const Image *image, uint32_t rva, void *out, size_t size) {
    if (rva >= image->image_size || size > image->image_size - rva) return 0;
    return image->read(image->context, image->base + rva, out, size);
}

static int name_char_allowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-' || c == '+';
}

static void read_export_name(const Image *image, const IMAGE_DATA_DIRECTORY *directory, char *out, size_t size) {
    IMAGE_EXPORT_DIRECTORY exports;
    size_t used = 0;
    out[0] = 0;
    if (!directory->VirtualAddress || !read_rva(image, directory->VirtualAddress, &exports, sizeof(exports))) return;
    for (uint32_t i = 0; used + 1 < size; ++i) {
        char c;
        if (!read_rva(image, exports.Name + i, &c, 1) || !c) break;
        if (c == '\\' || c == '/' || c == ':') {
            used = 0;
            continue;
        }
        out[used++] = name_char_allowed(c) ? c : '_';
    }
    out[used] = 0;
}

static int resource_entry(const Image *image, uint32_t directory, int want_id, uint32_t id, uint32_t *offset) {
    IMAGE_RESOURCE_DIRECTORY header;
    if (!read_rva(image, directory, &header, sizeof(header))) return 0;
    const uint32_t count = (uint32_t)header.NumberOfNamedEntries + header.NumberOfIdEntries;
    for (uint32_t i = 0; i < count && i < MAX_RESOURCE_ENTRIES; ++i) {
        IMAGE_RESOURCE_DIRECTORY_ENTRY entry;
        if (!read_rva(image, directory + (uint32_t)sizeof(header) + i * (uint32_t)sizeof(entry), &entry, sizeof(entry)))
            return 0;
        if (want_id && (entry.Name & 0x80000000u ? 1 : entry.Name != id)) continue;
        *offset = entry.OffsetToData;
        return 1;
    }
    return 0;
}

/* Resource tree: type RT_VERSION, then its first name, then its first language. */
static int read_file_version(const Image *image, const IMAGE_DATA_DIRECTORY *directory, uint16_t version[4]) {
    const uint32_t root = directory->VirtualAddress;
    uint32_t offset;
    if (!root || !resource_entry(image, root, 1, 16, &offset) || !(offset & 0x80000000u)) return 0;
    if (!resource_entry(image, root + (offset & 0x7fffffffu), 0, 0, &offset) || !(offset & 0x80000000u)) return 0;
    if (!resource_entry(image, root + (offset & 0x7fffffffu), 0, 0, &offset) || (offset & 0x80000000u)) return 0;
    IMAGE_RESOURCE_DATA_ENTRY data;
    if (!read_rva(image, root + offset, &data, sizeof(data))) return 0;
    uint32_t words[VERSION_SCAN_BYTES / 4];
    const uint32_t bytes = data.Size < sizeof(words) ? data.Size & ~3u : (uint32_t)sizeof(words);
    if (bytes < 16 || !read_rva(image, data.OffsetToData, words, bytes)) return 0;
    for (uint32_t i = 0; i + 3 < bytes / 4; ++i) {
        if (words[i] != VERSION_SIGNATURE) continue;
        version[0] = (uint16_t)(words[i + 2] >> 16);
        version[1] = (uint16_t)words[i + 2];
        version[2] = (uint16_t)(words[i + 3] >> 16);
        version[3] = (uint16_t)words[i + 3];
        return 1;
    }
    return 0;
}

int sos_host_module_parse(uintptr_t base, SosHostModuleRead read, void *context, SosHostModule *out) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!out || !read) return 0;
    if (!read(context, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x10000)
        return 0;
    if (!read(context, base + (uintptr_t)dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !nt.OptionalHeader.SizeOfImage)
        return 0;
    const Image image = {base, nt.OptionalHeader.SizeOfImage, read, context};
    out->base = base;
    out->image_size = nt.OptionalHeader.SizeOfImage;
    out->timestamp = nt.FileHeader.TimeDateStamp;
    out->has_version = 0;
    out->version[0] = out->version[1] = out->version[2] = out->version[3] = 0;
    const uint32_t directories = nt.OptionalHeader.NumberOfRvaAndSizes;
    out->name[0] = 0;
    if (directories > IMAGE_DIRECTORY_ENTRY_EXPORT)
        read_export_name(&image, &nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT], out->name,
                         sizeof(out->name));
    if (directories > IMAGE_DIRECTORY_ENTRY_RESOURCE)
        out->has_version =
            read_file_version(&image, &nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE], out->version);
    return 1;
}

static int read_process(void *context, uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    (void)context;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, out, size, &got) && got == size;
}

int sos_host_module_at(uintptr_t address, SosHostModule *out) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((LPCVOID)address, &info, sizeof(info)) || info.Type != MEM_IMAGE || !info.AllocationBase) return 0;
    if (!sos_host_module_parse((uintptr_t)info.AllocationBase, read_process, NULL, out)) return 0;
    return address - out->base < out->image_size;
}

typedef struct {
    char *data;
    size_t size, used;
} Out;

static void put_char(Out *out, char c) {
    if (out->used + 1 >= out->size) return;
    out->data[out->used++] = c;
    out->data[out->used] = 0;
}

static void put_str(Out *out, const char *s) {
    while (*s) put_char(out, *s++);
}

static void put_hex(Out *out, uint64_t value) {
    char digits[16];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (value);
    put_str(out, "0x");
    while (n) put_char(out, digits[--n]);
}

static void put_dec(Out *out, uint32_t value) {
    char digits[10];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) put_char(out, digits[--n]);
}

size_t sos_host_module_format(const SosHostModule *module, uintptr_t address, char *data, size_t size) {
    Out out = {data, size, 0};
    if (!data || !size) return 0;
    data[0] = 0;
    put_str(&out, "module=");
    put_str(&out, module->name[0] ? module->name : "unnamed");
    put_str(&out, " module_base=");
    put_hex(&out, module->base);
    put_str(&out, " module_offset=");
    put_hex(&out, address - module->base);
    put_str(&out, " module_size=");
    put_hex(&out, module->image_size);
    put_str(&out, " file_version=");
    if (module->has_version) {
        for (int i = 0; i < 4; ++i) {
            if (i) put_char(&out, '.');
            put_dec(&out, module->version[i]);
        }
    } else {
        put_str(&out, "unknown");
    }
    put_str(&out, " timestamp=");
    put_hex(&out, module->timestamp);
    return out.used;
}
