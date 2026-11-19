/* Offline: no online service is linked; the runtime answers as an offline console. */
#include "runtime.h"

void runtime_online_configure(const char *settings_file, const char *user_dir, const char *build) {
    (void)settings_file; (void)user_dir; (void)build;
}
int runtime_online_active(void) { return 0; }
void runtime_online_leave(void) {}
int runtime_online_status(void) { return 0; }
uintptr_t runtime_online_resolve(const char *name) { (void)name; return 0; }
void runtime_online_pump(void) {}
