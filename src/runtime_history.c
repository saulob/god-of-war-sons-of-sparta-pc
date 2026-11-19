/* Small fixed ring of the last platform transitions, printed only at a fatal STOP. */
#include "runtime.h"
#include "startup_phase.h"
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define HISTORY_ENTRIES 32
#define HISTORY_TEXT 56
typedef struct {
    char api[HISTORY_TEXT];
    const char *library;
    uintptr_t caller;
    int32_t thread;
    uint64_t ms;
} HistoryEntry;
static HistoryEntry entries[HISTORY_ENTRIES];
static _Atomic unsigned next_entry;

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

void runtime_history_note(const char *api, const char *library, uintptr_t caller) {
    const unsigned slot = atomic_fetch_add_explicit(&next_entry, 1, memory_order_relaxed);
    HistoryEntry *entry = &entries[slot % HISTORY_ENTRIES];
    snprintf(entry->api, sizeof(entry->api), "%s", api ? api : "?");
    entry->library = library;
    entry->caller = caller;
    entry->thread = runtime_thread_id_current();
    entry->ms = now_ms();
}

void runtime_history_dump(void) {
    const unsigned total = atomic_load_explicit(&next_entry, memory_order_acquire);
    if (!total) return;
    const unsigned first = total > HISTORY_ENTRIES ? total - HISTORY_ENTRIES : 0;
    fprintf(stderr, "Recent PS4 calls (%u):\n", total < HISTORY_ENTRIES ? total : HISTORY_ENTRIES);
    const uint64_t now = now_ms();
    for (unsigned i = first; i < total; ++i) {
        const HistoryEntry *entry = &entries[i % HISTORY_ENTRIES];
        fprintf(stderr, "  %s (%s) thread %d %llu ms ago", entry->api, entry->library ? entry->library : "?",
                entry->thread, (unsigned long long)(now >= entry->ms ? now - entry->ms : 0));
        if (entry->caller) {
            char where[160];
            runtime_describe_guest_address(entry->caller, where, sizeof(where));
            fprintf(stderr, ", caller %s", where);
        }
        fprintf(stderr, "\n");
    }
}
