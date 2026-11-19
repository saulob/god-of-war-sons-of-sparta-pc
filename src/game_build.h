#ifndef SOS_GAME_BUILD_H
#define SOS_GAME_BUILD_H
/* Supported game builds, keyed by eboot SHA-256 (the finished image's eboot, after the update layer).
 * Mirrors scripts/game_profiles.py; a build is only accepted when its eboot hash matches. */
static const struct {
    const char *title_id;
    const char *version;
    const char *label;
    const char *eboot_sha256;
} sos_game_builds[] = {
    {"CUSA57767", "1.00", "God of War: Sons of Sparta (USA) 1.00",
     "19e1759adbb8038164bc9c4c40af876ca68fea7c3442034d2b665bbdb9be4a31"},
    {"CUSA57767", "1.01", "God of War: Sons of Sparta (USA) 1.01",
     "aafe8a2ab8e55e0044fe371248749026c494a07d6864fbb6a7fa89fe39999d58"},
};
#endif
