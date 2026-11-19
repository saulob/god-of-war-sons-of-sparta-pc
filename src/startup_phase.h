/* The stage the process is in and the display and video context, kept for the crash report. */
#ifndef SOS_STARTUP_PHASE_H
#define SOS_STARTUP_PHASE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `phase` must outlive the process. Nothing is logged. */
void sos_startup_phase(const char *phase);
const char *sos_startup_phase_current(void);

/* A bounded log line "Checkpoint: +S s thread N: text" for a boundary of the startup, also kept for the crash report. */
void sos_checkpoint(const char *format, ...) __attribute__((format(__printf__, 1, 2)));
/* Kept for the crash report only. */
void sos_checkpoint_note(const char *text);
/* The most recent checkpoint text, or "" before the first one. */
const char *sos_checkpoint_last(void);

/* Names the calling thread for the crash report (a lock-free table of the first 128 threads). */
void sos_thread_label_set(const char *name);
/* The label of a thread, or "" when it has none. */
const char *sos_thread_label(unsigned long thread);
unsigned long sos_thread_id(void);

/* Copies the strings; they may be changed from any thread. */
void sos_crash_context_set_display(int width, int height, const char *resolution, const char *aspect_ratio);
void sos_crash_context_set_video(const char *path);

/* "phase=..., render 3360x1440 (1440p) 21:9, video=...": no allocation, no locks, safe in a fatal handler. */
size_t sos_crash_context_describe(char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
