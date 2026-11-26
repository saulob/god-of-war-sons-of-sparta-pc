#ifndef SOS_RUNTIME_H
#define SOS_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in sos_longjmp
 * (gpu/shim/dcp_toggles.h uses the same buffer type). */
typedef sos_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) sos_setjmp(buffer)
#else
#include <setjmp.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
#if defined(_WIN32) && !defined(__cplusplus)
/* Normal output is buffered to a complete line per thread, so reports made of several calls stay together. */
int runtime_log_printf(const char *format, ...) __attribute__((format(__printf__, 1, 2)));
#define printf runtime_log_printf
#endif
void runtime_log_exit_request(const char *api, int status, uintptr_t caller);
/* Ends the process without atexit handlers or C++ static destructors: those would tear down the video core under the
 * window, GPU, audio and guest threads, which keep running. */
static inline __attribute__((noreturn)) void runtime_exit(int status) {
    runtime_log_exit_request("runtime_exit",status,(uintptr_t)__builtin_return_address(0));
    fflush(NULL);
    _Exit(status);
}
void runtime_start(uint64_t capabilities);
void runtime_set_guest_image(const void *base, uint64_t size);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* Windows: commits the guest page holding `address` on its first use; 1 when the access (0 read, 1 write, 8 execute,
 * as the access violation reports it) can be retried. */
int runtime_memory_commit_fault(uintptr_t address, int access);
/* Windows: 1 when `address` faulted inside a partial map/unmap of its view and the access can be retried. */
int runtime_memory_wait_carve(uintptr_t address);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
uint64_t runtime_add_module_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_remove_module_tls(uint64_t module);
void *runtime_module_tls_addr(uint64_t module, uint64_t offset);
void runtime_module_tls_release(void);
uint64_t runtime_tls_generation(void);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
#ifndef _WIN32
/* Calls a guest entry on [stack, stack+size); the calling thread's guest stack bounds are that range while it runs. */
void *runtime_thread_enter_on_stack(void *entry, void *arg0, void *arg1, void *stack, size_t size);
#endif
uint32_t runtime_thread_tcb_offset(void);
uint32_t runtime_thread_thread_offset(void);
void runtime_thread_reserve_tls(void);
int32_t *runtime_errno(void);
int runtime_thread_cancel_pending(void);
void runtime_thread_test_cancel(const char *point);
int runtime_thread_wait_enter(void (*wake)(void *, void *), void *first, void *second);
void runtime_thread_wait_begin(const char *point, void (*wake)(void *, void *), void *first, void *second);
void runtime_thread_wait_end(void);
void runtime_thread_sleep_ns(uint64_t ns);
void runtime_thread_wake_condition(void *cond, void *mutex);
/* SOS_BOOT_DIAGNOSTIC: the calling thread's current blocking wait (NULL ends it); `point` must be a literal. */
void runtime_thread_track_waits(int enabled);
int runtime_thread_tracking_waits(void);
void runtime_thread_note_wait(const char *point, uintptr_t object);
void runtime_thread_diag_dump(void);
void runtime_sema_diag_dump(void);
void runtime_signal_diag_dump(void);
/* Where the red-zone analysis is kept between runs (none: always analyzed). */
void runtime_redzone_set_cache(const char *directory);
/* Guest bytes whose value may differ between runs of the same build (relocated addresses, the TCB slot, option
 * values), set before runtime_redzone_install. */
void runtime_redzone_add_volatile(uint64_t offset, uint64_t length);
/* Image offsets past the image's end that guest jumps may target as the image's own code (the CPU compatibility
 * stubs), set before runtime_redzone_install. */
void runtime_redzone_allow_branches(uint64_t begin, uint64_t end);
uint64_t runtime_redzone_install(unsigned char *image, uint64_t size, const uint64_t *segments, uint64_t count);
void runtime_redzone_after_fault(void *context);
int runtime_redzone_site(uintptr_t rip, uint64_t *site);
/* The start of the guest function holding image offset `offset`, from the red-zone analysis. */
int runtime_redzone_function(uint64_t offset, uint64_t *begin);
uintptr_t runtime_sema_resolve(const char *name);
uintptr_t runtime_fiber_resolve(const char *name);
void runtime_fiber_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
/* AudioOut for the runtime's other audio libraries: the paced SDL3 path the game's own ports use, at 48 kHz. */
int32_t runtime_audio_out_init(void);
int32_t runtime_audio_out_open(int32_t type, uint32_t frames, uint32_t param);
int32_t runtime_audio_out_output(int32_t handle, const void *data);
int32_t runtime_audio_out_close(int32_t handle);
uintptr_t runtime_audio3d_resolve(const char *name);
void runtime_audio3d_report(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_json_resolve(const char *name);
void runtime_json_report(void);
uintptr_t runtime_sos_resolve(const char *name);
uintptr_t runtime_extra_resolve(const char *name);
uintptr_t runtime_umtx_resolve(const char *name);
uintptr_t runtime_module_resolve(const char *name);
int32_t runtime_module_register_linked(const char *path, const void *base, uint64_t size, uint64_t image_offset);
int32_t runtime_module_register_main(const char *path, const void *base, uint64_t size);
void runtime_module_linked_started(uint64_t image_offset, int32_t result);
ABI int32_t runtime_module_load_start(const char *path, uint64_t args, void *argp, uint32_t flags, void *opt, int32_t *result);
ABI int32_t runtime_module_dlsym(int32_t handle, const char *name, void **out);
int runtime_module_describe_address(uintptr_t address, char *out, size_t size);
/* Lock-free: address lies in a PRX loaded at run time. */
int runtime_module_in_loaded_code(uintptr_t address);
void runtime_symbol_to_nid(const char *symbol, char out[12]);
void runtime_describe_guest_address(uintptr_t address, char *out, size_t size);
/* Symbolic name of a PS4 libc debug-stop code (sceKernelDebugRaiseException), "unknown" otherwise. */
const char *runtime_debug_stop_name(uint32_t code);
const char *runtime_module_operation(void);
void runtime_sos_set_neo_mode(int on);
/* Direct memory on top of the retail pool, for the larger GPU heap of a larger render target; before the first query. */
void runtime_memory_set_direct_extra(uint64_t bytes);
void runtime_reset_process_clock(void);
uint64_t runtime_monotonic_ms(void);
/* The last guest memory maps and unmaps as "Runtime: memory op" lines; a range holding `focus` is marked. */
size_t runtime_memory_recent(char *out, size_t size, uintptr_t focus);
void runtime_set_language(int ps4_language);
void runtime_user_login(int player);
int runtime_user_player(int32_t user);
uintptr_t runtime_font_resolve(const char *name);
/* The accented letters composed into the font atlas, `cell` pixels square; the data must outlive the process's fonts. */
uintptr_t runtime_eventflag_resolve(const char *name);
/* The sons_of_sparta.ini whose keyboard bindings the pad reads, once, at the first sample. */
void runtime_pad_set_settings(const char *settings_file);
/* 1 when the slot is Player 1 or has a live gamepad; 0 for a slot the game may drive with AI. */
int runtime_pad_slot_human(int slot);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_move_resolve(const char *name);
void runtime_move_report(void);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_posix_socket_resolve(const char *name);
int64_t runtime_socket_close_file(int fd);
int64_t runtime_socket_read_file(int fd, void *buffer, uint64_t size);
int64_t runtime_socket_write_file(int fd, const void *buffer, uint64_t size);
int64_t runtime_socket_fcntl_file(int fd, int command, intptr_t argument);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
uintptr_t runtime_users_resolve(const char *name);
/* 1 once the game polled sceUserServiceGetEvent: its user manager knows the initial user. */
int runtime_user_events_polled(void);
/* Queues one SceSystemServiceEvent (eventType @0, data @4; at most 0xc0 bytes) for sceSystemServiceReceiveEvent; 0 when
 * the queue is full. */
int runtime_system_event_push(const void *event, size_t size);
/* Online play for this process (runtime_online.cpp): reads the settings, the profile's online identity and the game
 * build ("CUSA00062-1.00"; NULL for an unknown build, which stays offline), then configures the NP layer. Before the
 * imports are bound. */
void runtime_online_configure(const char *settings_file, const char *user_dir, const char *build);
int runtime_online_active(void);
/* The process is ending: the room and the lobby link are left cleanly (a second at most), so the squad and the friends see
 * the player go at once instead of after the lobby's grace period. */
void runtime_online_leave(void);
/* What the network status box shows: ONLINE_STATUS_* (online_status.h); logs each change. */
int runtime_online_status(void);
/* Matching2 and the game's P2P socket while online play is on; 0 otherwise or for another import. */
uintptr_t runtime_online_resolve(const char *name);
/* Lobby messages, the endpoint exchange and the queued Matching2 callbacks, on the thread calling sceNpCheckCallback. */
void runtime_online_pump(void);
/* The initial user signed in as `online_id` (16 characters at most), the network connected at `ip` (host byte
 * order). Off: runtime_np_resolve answers nothing and runtime_services.c keeps the offline behaviour. */
void runtime_np_configure(int enabled, uint64_t account, const char *online_id, uint32_t ip);
int runtime_np_enabled(void);
/* Called at the end of every sceNpCheckCallback while online play is on. */
void runtime_np_set_pump(void (*pump)(void));
/* The game was told its user signed in. */
int runtime_np_signed_in(void);
uintptr_t runtime_np_resolve(const char *name);
/* libSceNpToolkit2 imports of UnityNpToolkit2.prx, resolved regardless of online play; names are "NID#libSceNpToolkit2". */
uintptr_t runtime_np_toolkit2_resolve(const char *name);
const char *runtime_np_toolkit2_symbol(const char *name);
void runtime_np_toolkit2_reset(void);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
int runtime_file_mount_read_only(const char *guest, const char *host);
int runtime_file_is_open(int fd);
void runtime_file_unmount(const char *guest);
typedef struct {
    uint64_t offset, size;
    const void *data;
} RuntimeFilePatch;
/* The guest sees `guest` as `length` bytes: the host file, zeros past its end, and the patches on top. The patches'
 * data is borrowed and must outlive the process. Before the guest starts only. A path registered again with the same
 * length gets the new patches on top of the earlier ones. -1 with nothing registered when the table is full, the path
 * already has an overlay of another length or a patch falls outside `length`. */
int runtime_file_overlay(const char *guest, uint64_t length, const RuntimeFilePatch *patches, size_t count);
/* The guest sees the file at `guest` (/app0/Media/resources.assets) as the host file `host`: that exact path only, before the
 * guest starts. -1 when the table is full or a path does not fit. */
int runtime_file_alias(const char *guest, const char *host);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
/* Called after every successful read with the guest path, the file offset and the size (Discord presence). It runs on
 * the reading thread and must not block. */
void runtime_file_set_read_observer(void (*observe)(const char *guest, uint64_t offset, uint64_t size));
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
#ifndef _WIN32
/* Stacks below 1 TiB with a guard page at the bottom; released stacks are reused. */
void *runtime_stack_acquire(size_t size);
void runtime_stack_release(void *stack, size_t size);
#endif
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
/* Creates the guest memory pool and commits a first page; nonzero when that fails. */
int runtime_memory_prepare(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
void runtime_thread_attach_host(const char *name);
int32_t runtime_thread_id_current(void);
const char *runtime_thread_current_name(void);
void runtime_history_note(const char *api, const char *library, uintptr_t caller);
void runtime_history_dump(void);
/* Name of a guest thread handle, or NULL when it is not a live thread. */
const char *runtime_thread_name(void *handle);
uint64_t runtime_memory_flexible_available(void);
/* Guest signal state of one thread (FreeBSD sigset layout: bit sig-1 of 128). Accessed atomically. */
typedef struct RuntimeThreadSignals {
    uint32_t mask[4];
    uint32_t pending[128];
    uint32_t wait_set[4];
    void *alt_stack;
    uint64_t alt_size;
    uint32_t alt_flags;
    int in_sigwait, in_sigsuspend, suspend_interrupted;
} RuntimeThreadSignals;
/* NULL handle: the calling thread. NULL result: not a live guest thread. */
RuntimeThreadSignals *runtime_thread_signals(void *handle);
void *runtime_thread_native_handle(void *handle);
uintptr_t runtime_signal_resolve(const char *name);
/* Delivers signals queued for the calling thread before it was able to receive them. */
void runtime_signal_deliver_pending(void);
/* 1 when the calling thread has a pending signal it does not block. */
int runtime_signal_deliverable(void);
/* Wakes the runtime wait `handle` is blocked in, if any, so it reaches its signal delivery point. */
void runtime_thread_kick(void *handle);
void runtime_thread_signal_scan(int (*visit)(RuntimeThreadSignals *, void *native));
/* 1 when `address` is guest code: the linked image, a loaded guest module or a relocated red-zone stub. Lock-free. */
int runtime_is_guest_code(uintptr_t address);
int runtime_module_contains(uintptr_t address);
int runtime_redzone_contains(uintptr_t address);
#ifdef __cplusplus
}
#endif
#endif
