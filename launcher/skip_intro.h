/* Whether Skip Intro is verified for a build (the loader asks before it starts the video core). */
#ifndef SOS_SKIP_INTRO_H
#define SOS_SKIP_INTRO_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the boot image folder belongs to a build whose startup video Skip Intro handles. */
int sos_skip_intro_supported(const char* image_dir);

#ifdef __cplusplus
}
#endif

#endif
