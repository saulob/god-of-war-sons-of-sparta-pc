/* DEBUG_SAULO: temporary True Ultrawide investigation. Publishes the gameplay camera position to the video core's
 * frame captures. Remove after the diagnosis. */
#ifndef SOS_UW_PROBE_H
#define SOS_UW_PROBE_H

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SOS_UW_PROBE_SYSV __attribute__((sysv_abi))
#define SOS_UW_PROBE_STOLEN 6
#define SOS_UW_PROBE_PREFIX 20
#define SOS_UW_PROBE_STUB_BYTES 64

/* Cinemachine CameraState as CinemachineBrain.PushStateToUnityCamera(ref CameraState) receives it: lens ortho size,
 * RawPosition and PositionCorrection (the camera is at their sum). */
#define SOS_UW_PROBE_ORTHO 0x04
#define SOS_UW_PROBE_RAW_POSITION 0x4c
#define SOS_UW_PROBE_CORRECTION 0x78

typedef void (*SosUwProbePublish)(float x, float y, float ortho);
typedef uint32_t (*SosUwProbeSerial)(void);

/* Image offset of PushStateToUnityCamera in a supported build, 0 otherwise. */
uint32_t sos_uw_probe_site(const char *build_key);
int sos_uw_probe_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stubs, size_t stubs_capacity,
                         char *message, size_t message_size);
void sos_uw_probe_arm(SosUwProbePublish publish, SosUwProbeSerial serial);

SOS_UW_PROBE_SYSV void sos_uw_probe_on_camera(void *brain, const void *state);

#ifdef __cplusplus
}
#endif

#endif
