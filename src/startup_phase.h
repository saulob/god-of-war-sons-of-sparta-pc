/* The stage the process is in and the display and video context, kept for the crash report. */
#ifndef SOS_STARTUP_PHASE_H
#define SOS_STARTUP_PHASE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `phase` must outlive the process. A change of phase logs one bounded, flushed line
 * "Stage: <phase> started +T ms (after <previous>, D ms)"; setting the same phase again logs nothing. */
void sos_startup_phase(const char *phase);
const char *sos_startup_phase_current(void);
/* The phase before the current one, or the last stage reported completed; "" before the first. */
const char *sos_startup_phase_completed(void);
/* Milliseconds since the current phase started. */
double sos_startup_phase_elapsed_ms(void);
/* "Stage: <stage> completed in D ms: detail" or "Stage: <stage> failed in D ms: detail", flushed; the duration is given
 * when <stage> is the current phase. */
void sos_startup_stage_result(const char *stage, int ok, const char *format, ...) __attribute__((format(__printf__, 3, 4)));
/* "Stage event: text +T ms", flushed: one-off facts of the start (PLAY and its input, the handoff). */
void sos_startup_event(const char *format, ...) __attribute__((format(__printf__, 1, 2)));
/* How stage lines are pushed out; without a hook stdout is flushed (into the log pipe, written to the file at once by the
 * log thread). Never pass a flush that waits for the file: phases change on frame-critical threads. Tests use it. */
void sos_startup_set_flush(void (*flush)(void));

/* The diagnostic watchdog: reports a phase that lasts longer than its budget ("Stage watchdog: ..."), at 1x, 2x, 4x and
 * 8x the budget, and never stops or changes the process. Phases that wait for the player have no budget. */
void sos_startup_watchdog_start(void);
unsigned sos_startup_phase_budget_seconds(const char *phase);
/* 1 when the watchdog reports now; `watched` and `reports` are its state (tests drive it with synthetic time). */
int sos_startup_watchdog_poll(const char *phase, double elapsed_ms, const char **watched, unsigned *reports);

/* PLAY: the baseline of the checkpoints and startup milestones. */
void sos_startup_play(void);
/* In the game process the launcher started: PLAY is the launcher's. */
void sos_startup_inherit_play(void);
/* A bounded log line "Startup: text +X ms (process +Y ms)", X since PLAY, or since the process started when no launcher
 * ran. For the few boundaries of the way to the first frame: each caller logs once. */
void sos_startup_milestone(const char *format, ...) __attribute__((format(__printf__, 1, 2)));

/* A bounded log line "Checkpoint: +S s thread N: text" (S since PLAY, as above), also kept for the crash report. */
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
