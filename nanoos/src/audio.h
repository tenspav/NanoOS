#ifndef NANOOS_AUDIO_H
#define NANOOS_AUDIO_H
#include <stdint.h>
/* codec: last value of the probed register (-1 = no answer); frames:
 * stereo frames written; playing: tone in progress */
struct audio_state { int codec; uint32_t frames, ms; unsigned playing; };
extern struct audio_state audio;
int audio_codec_read(unsigned reg);
void audio_tone(uint32_t ms);   /* start a 440 Hz square tone */
void audio_service(void);       /* call every pass while a tone plays */
#endif
