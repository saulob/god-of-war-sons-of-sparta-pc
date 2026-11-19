#include "startup_phase.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#define MAX_CHECKPOINT_LINES 96

static const char *current_phase = "process start";
static int display_width, display_height;
static char display_resolution[16], display_aspect[16], video_path[160];
static char last_checkpoint[2][160];
static unsigned checkpoint_count, published_checkpoint;

static void copy_text(char *to, size_t size, const char *from) {
    size_t length = from ? strlen(from) : 0;
    if (length >= size) length = size - 1;
    memcpy(to, from, length);
    to[length] = 0;
}

void sos_startup_phase(const char *phase) {
    __atomic_store_n(&current_phase, phase, __ATOMIC_RELEASE);
}

const char *sos_startup_phase_current(void) {
    return __atomic_load_n(&current_phase, __ATOMIC_ACQUIRE);
}

void sos_crash_context_set_display(int width, int height, const char *resolution, const char *aspect_ratio) {
    display_width = width;
    display_height = height;
    copy_text(display_resolution, sizeof(display_resolution), resolution);
    copy_text(display_aspect, sizeof(display_aspect), aspect_ratio);
}

void sos_crash_context_set_video(const char *path) {
    copy_text(video_path, sizeof(video_path), path);
}

#define THREAD_LABELS 128
static struct { unsigned long thread; char name[32]; } thread_labels[THREAD_LABELS];
static unsigned thread_label_count;

static unsigned long current_thread(void);

unsigned long sos_thread_id(void) { return current_thread(); }

void sos_thread_label_set(const char *name) {
    const unsigned long self = current_thread();
    for (unsigned i = 0; i < THREAD_LABELS && i < __atomic_load_n(&thread_label_count, __ATOMIC_ACQUIRE); ++i)
        if (thread_labels[i].thread == self) {
            copy_text(thread_labels[i].name, sizeof(thread_labels[i].name), name);
            return;
        }
    const unsigned slot = __atomic_fetch_add(&thread_label_count, 1, __ATOMIC_ACQ_REL);
    if (slot >= THREAD_LABELS) return;
    copy_text(thread_labels[slot].name, sizeof(thread_labels[slot].name), name);
    __atomic_store_n(&thread_labels[slot].thread, self, __ATOMIC_RELEASE);
}

const char *sos_thread_label(unsigned long thread) {
    const unsigned count = __atomic_load_n(&thread_label_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < THREAD_LABELS && i < count; ++i)
        if (__atomic_load_n(&thread_labels[i].thread, __ATOMIC_ACQUIRE) == thread) return thread_labels[i].name;
    return "";
}

static unsigned long current_thread(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

#ifdef _WIN32
#define PLAY_VARIABLE "SOS_STARTUP_PLAY"
static int64_t play_ticks; /* QueryPerformanceCounter at PLAY; QPC is the same clock in every process */
static int inherit_play;

static int64_t now_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
static double ticks_ms(int64_t ticks) {
    static int64_t frequency;
    if (!frequency) {
        LARGE_INTEGER hz;
        QueryPerformanceFrequency(&hz);
        frequency = hz.QuadPart;
    }
    return (double)ticks * 1000.0 / (double)frequency;
}
static double process_ms(void) {
    FILETIME created, unused[3], now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &unused[0], &unused[1], &unused[2])) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    const uint64_t from = (uint64_t)created.dwHighDateTime << 32 | created.dwLowDateTime;
    const uint64_t to = (uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime;
    return to > from ? (double)(to - from) / 10000.0 : 0;
}

void sos_startup_play(void) {
    const int64_t ticks = now_ticks();
    __atomic_store_n(&play_ticks, ticks, __ATOMIC_RELEASE);
    char value[24];
    snprintf(value, sizeof(value), "%lld", (long long)ticks);
    SetEnvironmentVariableA(PLAY_VARIABLE, value);
}

void sos_startup_inherit_play(void) { inherit_play = 1; }

/* Milliseconds since PLAY, -1 when there was none in this process or the launcher's. */
static double since_play_ms(void) {
    int64_t ticks = __atomic_load_n(&play_ticks, __ATOMIC_ACQUIRE);
    if (!ticks) {
        if (!inherit_play) return -1;
        char value[24];
        const DWORD length = GetEnvironmentVariableA(PLAY_VARIABLE, value, sizeof(value));
        ticks = length && length < sizeof(value) ? _strtoi64(value, NULL, 10) : 0;
        if (ticks <= 0 || ticks > now_ticks()) return -1;
        __atomic_store_n(&play_ticks, ticks, __ATOMIC_RELEASE);
    }
    return ticks_ms(now_ticks() - ticks);
}
#else
static double now_ms(void) {
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return (double)spec.tv_sec * 1000.0 + (double)spec.tv_nsec / 1e6;
}
static double play_at = -1, process_at = -1;
static double process_ms(void) {
    if (process_at < 0) process_at = now_ms();
    return now_ms() - process_at;
}
void sos_startup_play(void) { play_at = now_ms(); }
void sos_startup_inherit_play(void) {}
static double since_play_ms(void) { return play_at < 0 ? -1 : now_ms() - play_at; }
#endif

static double elapsed_seconds(void) {
    const double play = since_play_ms();
    return (play >= 0 ? play : process_ms()) / 1000.0;
}

#define MAX_MILESTONES 32
static unsigned milestone_count;

void sos_startup_milestone(const char *format, ...) {
    const double process = process_ms(), play = since_play_ms();
    if (__atomic_add_fetch(&milestone_count, 1, __ATOMIC_RELAXED) > MAX_MILESTONES) return;
    char text[120];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (play >= 0) printf("Startup: %s +%.0f ms (process +%.0f ms)\n", text, play, process);
    else printf("Startup: %s +%.0f ms since the process started (no PLAY)\n", text, process);
}

void sos_checkpoint_note(const char *text) {
    const unsigned slot = (__atomic_load_n(&published_checkpoint, __ATOMIC_ACQUIRE) + 1) & 1;
    char note[160];
    snprintf(note, sizeof(note), "[thread %lu] %s", current_thread(), text ? text : "");
    copy_text(last_checkpoint[slot], sizeof(last_checkpoint[slot]), note);
    __atomic_store_n(&published_checkpoint, slot, __ATOMIC_RELEASE);
}

const char *sos_checkpoint_last(void) {
    return last_checkpoint[__atomic_load_n(&published_checkpoint, __ATOMIC_ACQUIRE)];
}

void sos_checkpoint(const char *format, ...) {
    char text[140];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    sos_checkpoint_note(text);
    if (__atomic_add_fetch(&checkpoint_count, 1, __ATOMIC_RELAXED) <= MAX_CHECKPOINT_LINES)
        printf("Checkpoint: +%.3f s thread %lu: %s\n", elapsed_seconds(), current_thread(), text);
}

static size_t append(char *out, size_t size, size_t at, const char *text) {
    while (*text && at + 1 < size) out[at++] = *text++;
    out[at < size ? at : size - 1] = 0;
    return at;
}

static size_t append_number(char *out, size_t size, size_t at, int value) {
    char digits[12];
    int count = 0;
    unsigned remaining = value < 0 ? 0u : (unsigned)value;
    do digits[count++] = (char)('0' + remaining % 10); while ((remaining /= 10) && count < 11);
    while (count) {
        char one[2] = {digits[--count], 0};
        at = append(out, size, at, one);
    }
    return at;
}

size_t sos_crash_context_describe(char *out, size_t size) {
    if (!size) return 0;
    out[0] = 0;
    size_t at = append(out, size, 0, "phase=");
    at = append(out, size, at, sos_startup_phase_current());
    at = append(out, size, at, ", render ");
    at = append_number(out, size, at, display_width);
    at = append(out, size, at, "x");
    at = append_number(out, size, at, display_height);
    at = append(out, size, at, " (");
    at = append(out, size, at, display_resolution);
    at = append(out, size, at, ") ");
    at = append(out, size, at, display_aspect);
    at = append(out, size, at, ", video=");
    at = append(out, size, at, video_path[0] ? video_path : "none");
    at = append(out, size, at, ", last checkpoint=");
    const char *note = last_checkpoint[__atomic_load_n(&published_checkpoint, __ATOMIC_ACQUIRE)];
    at = append(out, size, at, note[0] ? note : "none");
    return at;
}
