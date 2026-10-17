#ifndef NANOOS_AUDIO_H
#define NANOOS_AUDIO_H
#include <stdint.h>
/* id: codec registers 0x00..0x03, 0x06, 0x07, 0x0f, 0x220, 0x227, 0x229
 * and 0x2f (last; -1 = SPI timeout, FF = MISO undriven); w: play-mode
 * codec writes done (18 = all). Playback: passes = DMA passes queued,
 * under = restarts after a late audio_service(), frames = source frames
 * queued so far (stuck at 2048 for a 22.05 kHz source = no DMA request). */
#define AUDIO_RATE 44100u        /* I2S0 output: the codec as RetailOS set it */
#define AUDIO_STEREO 0x80000000u /* rate flag: interleaved L/R source */
#define AUDIO_NREG 11u
struct audio_state {
    int id[AUDIO_NREG];
    unsigned w;
    uint32_t passes, under, frames;
    int playing;
};
extern struct audio_state audio;
void audio_init(void);          /* probe, play the pack's test.pcm, log R82 */
/* Play s16le PCM (len bytes, 2-byte aligned, mono unless rate has
 * AUDIO_STEREO) at rate Hz (1..AUDIO_RATE*4, resampled to AUDIO_RATE),
 * replacing what is playing. The buffer is read while it plays. 1 = started.
 * audio_service() keeps it fed and must run at least every ~45 ms; if it
 * stops being called, the DMA chain ends by itself within ~0.1 s.
 * audio_stop() ends it at once; audio.playing = 0 once it has ended. */
int audio_play_pcm(const void *pcm, uint32_t len, uint32_t rate);
void audio_service(void);
void audio_stop(void);
#endif
