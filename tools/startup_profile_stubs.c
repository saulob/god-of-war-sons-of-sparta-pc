/* No-op startup profile and milestones for the headless tests that build single runtime sources. */
#include "startup_profile.h"

int sos_startup_profile_active(void) { return 0; }
int64_t sos_cost_begin(void) { return 0; }
void sos_cost_end(SosCost kind, int64_t start, uint64_t amount) { (void)kind; (void)start; (void)amount; }
void sos_startup_milestone(const char *format, ...) { (void)format; }
