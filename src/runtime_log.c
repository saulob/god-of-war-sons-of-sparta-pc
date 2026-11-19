#include "runtime_log.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static HANDLE pipe_read, direct_read, direct_pipe, log_file, echo_target;
static HANDLE normal_flushed, direct_flushed;
static volatile LONG64 echoed;
static CRITICAL_SECTION write_lock;
static CRITICAL_SECTION direct_lock;
static CRITICAL_SECTION line_lock;
static int locks_ready;
static char direct_log_path[4200];
static _Thread_local char line_buffer[8192];
static _Thread_local size_t line_used;

FILE *sos_stderr_stream(void) { return stdout; }

int runtime_log_printf(const char *format, ...) {
    if (!format) return 0;
    char text[4096];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0) return length;
    const size_t count = (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1;
    for (size_t i = 0; i < count; ++i) {
        if (line_used + 1 < sizeof(line_buffer)) line_buffer[line_used++] = text[i];
        if (text[i] == '\n' || line_used + 1 == sizeof(line_buffer)) {
            if (locks_ready) EnterCriticalSection(&line_lock);
            fwrite(line_buffer, 1, line_used, stdout);
            fflush(stdout);
            if (locks_ready) LeaveCriticalSection(&line_lock);
            line_used = 0;
        }
    }
    return length;
}

int runtime_log_write_direct(const char *text) {
    if (!text || !direct_pipe) return 0;
    const DWORD size = (DWORD)strlen(text);
    DWORD written = 0;
    if (locks_ready) EnterCriticalSection(&direct_lock);
    int ok = WriteFile(direct_pipe, text, size, &written, NULL) && written == size;
    const char marker = 0x1e;
    if (ok && direct_flushed) ok = WriteFile(direct_pipe, &marker, 1, &written, NULL) && written == 1;
    if (locks_ready) LeaveCriticalSection(&direct_lock);
    if (ok && direct_flushed) WaitForSingleObject(direct_flushed, 2000);
    return ok;
}

static void make_directories(const char *dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s", dir);
    for (char *p = path; *p; ++p) {
        if ((*p == '\\' || *p == '/') && p > path && p[-1] != ':') {
            char separator = *p;
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = separator;
        }
    }
    CreateDirectoryA(path, NULL);
}

static unsigned last_number(const char *dir) {
    char pattern[4200];
    snprintf(pattern, sizeof(pattern), "%s\\sons_of_sparta_*.log", dir);
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    unsigned last = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        unsigned number = 0;
        int used = 0;
        if (sscanf(found.cFileName, "sons_of_sparta_%u.log%n", &number, &used) == 1 && used > 0 && !found.cFileName[used] && number > last)
            last = number;
    } while (FindNextFileA(find, &found));
    FindClose(find);
    return last;
}

int runtime_log_create(const char *dir, const char *file, char *path, size_t size, void **handle) {
    if (file) {
        if (snprintf(path, size, "%s", file) >= (int)size) return 0;
        char *separator = strrchr(path, '\\');
        if (strrchr(path, '/') > separator) separator = strrchr(path, '/');
        if (separator) {
            char folder = *separator;
            *separator = 0;
            make_directories(path);
            *separator = folder;
        }
        *handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        return *handle != INVALID_HANDLE_VALUE;
    }
    make_directories(dir);
    for (unsigned number = last_number(dir) + 1, tries = 0; tries < 1000; ++number, ++tries) {
        if (snprintf(path, size, "%s\\sons_of_sparta_%03u.log", dir, number) >= (int)size) return 0;
        *handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (*handle != INVALID_HANDLE_VALUE) return 1;
        if (GetLastError() != ERROR_FILE_EXISTS) return 0;
    }
    return 0;
}

typedef struct { HANDLE source, flushed; } WriterArgs;
static void write_chunk(char *buffer, DWORD size) {
    if (!size) return;
    DWORD written = 0;
    EnterCriticalSection(&write_lock);
    if (log_file && log_file != INVALID_HANDLE_VALUE &&
        WriteFile(log_file, buffer, size, &written, NULL) && written == size)
        InterlockedAdd64(&echoed, size);
    if (echo_target && echo_target != INVALID_HANDLE_VALUE)
        WriteFile(echo_target, buffer, size, &written, NULL);
    LeaveCriticalSection(&write_lock);
}
static DWORD WINAPI writer_thread(LPVOID argument) {
    WriterArgs *args = argument;
    char buffer[65536];
    char *pending = malloc(1024 * 1024);
    size_t pending_size = 0;
    if (!pending) return 0;
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(args->source, buffer, sizeof(buffer), &read, NULL);
        if (!ok || !read) {
            if (!ok && GetLastError() == ERROR_BROKEN_PIPE) return 0;
            Sleep(5);
            continue;
        }
        for (DWORD i = 0; i < read; ++i) {
            if ((unsigned char)buffer[i] == 0x1e) {
                write_chunk(pending, (DWORD)pending_size);
                pending_size = 0;
                SetEvent(args->flushed);
                continue;
            }
            pending[pending_size++] = buffer[i];
            if (buffer[i] == '\n' || pending_size == 1024 * 1024) {
                write_chunk(pending, (DWORD)pending_size);
                pending_size = 0;
            }
        }
    }
}

static void attach_stream(FILE *stream, int fd, int std_handle) {
    if (_fileno(stream) < 0 && freopen("NUL", "w", stream)) setvbuf(stream, NULL, _IONBF, 0);
    const int stream_fd = _fileno(stream);
    if (stream_fd >= 0 && stream_fd != fd) _dup2(fd, stream_fd);
    SetStdHandle(std_handle, (HANDLE)_get_osfhandle(fd));
}

static int redirect_to_log(const char *path) {
    runtime_log_set_session_path(path);
    snprintf(direct_log_path, sizeof(direct_log_path), "%s", path);
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE), original = NULL;
    if (console && console != INVALID_HANDLE_VALUE && GetFileType(console) != FILE_TYPE_UNKNOWN)
        DuplicateHandle(GetCurrentProcess(), console, GetCurrentProcess(), &original, 0, FALSE, DUPLICATE_SAME_ACCESS);
    log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) return 0;
    HANDLE read_end = NULL, write_end = NULL;
    if (!CreatePipe(&read_end, &write_end, NULL, 65536)) {
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
        return 0;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&direct_read, &direct_pipe, NULL, 65536)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
        return 0;
    }
    SetHandleInformation(direct_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(direct_pipe, HANDLE_FLAG_INHERIT, 0);
    pipe_read = read_end;
    echo_target = original;
    if (!locks_ready) {
        InitializeCriticalSection(&write_lock);
        InitializeCriticalSection(&direct_lock);
        InitializeCriticalSection(&line_lock);
        locks_ready = 1;
    }
    normal_flushed = CreateEventA(NULL, FALSE, FALSE, NULL);
    direct_flushed = CreateEventA(NULL, FALSE, FALSE, NULL);
    static WriterArgs normal_args, direct_args;
    normal_args = (WriterArgs){pipe_read, normal_flushed};
    direct_args = (WriterArgs){direct_read, direct_flushed};
    HANDLE thread = normal_flushed && direct_flushed ? CreateThread(NULL, 0, writer_thread, &normal_args, 0, NULL) : NULL;
    HANDLE direct_thread = thread ? CreateThread(NULL, 0, writer_thread, &direct_args, 0, NULL) : NULL;
    if (!thread || !direct_thread) {
        if (thread) CloseHandle(thread);
        if (direct_thread) CloseHandle(direct_thread);
        CloseHandle(pipe_read);
        CloseHandle(direct_read);
        CloseHandle(direct_pipe);
        CloseHandle(write_end);
        CloseHandle(log_file);
        pipe_read = direct_read = direct_pipe = log_file = INVALID_HANDLE_VALUE;
        echo_target = NULL;
        return 0;
    }
    CloseHandle(thread);
    CloseHandle(direct_thread);
    fflush(NULL);
    const int fd = _open_osfhandle((intptr_t)write_end, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(write_end);
        return 0;
    }
    _dup2(fd, 1);
    _dup2(fd, 2);
    _close(fd);
    attach_stream(stdout, 1, STD_OUTPUT_HANDLE);
    attach_stream(stderr, 2, STD_ERROR_HANDLE);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (_fileno(stdin) < 0 && freopen("NUL", "r", stdin)) setvbuf(stdin, NULL, _IONBF, 0);
    return 1;
}

int runtime_log_start(const char *dir, const char *file, char *path, size_t size) {
    void *created;
    if (!runtime_log_create(dir, file, path, size, &created)) return 0;
    CloseHandle((HANDLE)created);
    return redirect_to_log(path);
}

int runtime_log_continue(const char *file, char *path, size_t size) {
    if (!file || GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES) return 0;
    if (snprintf(path, size, "%s", file) >= (int)size) return 0;
    return redirect_to_log(path);
}

void runtime_log_release(void) {
    if (!direct_log_path[0]) return;
    runtime_log_flush();
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);
    for (int fd = 1; fd <= 2; ++fd) {
        const int nul = _open("NUL", _O_WRONLY | _O_BINARY);
        if (nul < 0) break;
        if (nul != fd) _dup2(nul, fd);
        _close(nul);
    }
}

void runtime_log_flush(void) {
    fflush(NULL);
    if (!direct_pipe || !normal_flushed || !direct_flushed) return;
    fputc(0x1e, stdout);
    fflush(stdout);
    const char marker = 0x1e;
    DWORD written = 0;
    WriteFile(direct_pipe, &marker, 1, &written, NULL);
    WaitForSingleObject(normal_flushed, 2000);
    WaitForSingleObject(direct_flushed, 2000);
}
