/* Narrow UTF-8 paths must reach files under a non-ASCII folder with the production executable resources linked in. */
#include <stdio.h>
#include <sys/stat.h>
#include <windows.h>

static int failures;
static void check(const char *name, int condition) {
    printf("  [%s] %s\n", condition ? "ok" : "fail", name);
    failures += !condition;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    wchar_t folder[MAX_PATH], file[MAX_PATH + 16];
    char path[MAX_PATH * 3], moved[MAX_PATH * 3 + 8];
    swprintf(folder, MAX_PATH, L"%hs/João 日本", argv[1]);
    swprintf(file, MAX_PATH + 16, L"%ls/eboot.bin", folder);
    CreateDirectoryW(folder, NULL);
    FILE *f = _wfopen(file, L"wb");
    if (f) fclose(f);
    WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof(path), NULL, NULL);
    snprintf(moved, sizeof(moved), "%s.new", path);
    struct stat st;
    check("the test file exists", f != NULL);
    check("the process code page is UTF-8", GetACP() == CP_UTF8);
    FILE *g = fopen(path, "rb");
    check("fopen reaches a UTF-8 path", g != NULL);
    if (g) fclose(g);
    check("stat reaches a UTF-8 path", !stat(path, &st));
    check("MoveFileExA reaches a UTF-8 path", MoveFileExA(path, moved, MOVEFILE_REPLACE_EXISTING) != 0);
    printf("manifest tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
