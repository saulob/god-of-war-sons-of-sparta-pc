/* The kind of the physical controller, as the pad layer classifies it. The pad layer publishes it when the connected gamepads
 * change and the controller prompts read it, so neither calls into the other. */
#ifndef SOS_PAD_KIND_H
#define SOS_PAD_KIND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOS_PAD_NONE = 0,
    SOS_PAD_PLAYSTATION5 = 1,
    SOS_PAD_PLAYSTATION4 = 2,
    SOS_PAD_OTHER = 3,
    SOS_PAD_XBOX = 4
} SosPadKind;

void sos_pad_kind_publish(int pad_kind);

/* SOS_PAD_NONE until a kind is published. */
int sos_pad_kind_current(void);

#ifdef __cplusplus
}
#endif

#endif
