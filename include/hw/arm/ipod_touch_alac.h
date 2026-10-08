#ifndef HW_ARM_IPOD_TOUCH_ALAC_H
#define HW_ARM_IPOD_TOUCH_ALAC_H

#include "qemu/osdep.h"

/*
 * A decoder for 16-bit Apple Lossless (ALAC) frames, as the AMC model receives them: raw frames back to back without
 * their sizes. Each frame ends with an ID_END element, so the decoder reports where the next frame starts.
 */

#define ALAC_MAX_FRAME_LENGTH 4096
#define ALAC_MAX_CHANNELS     2

typedef struct ALACConfig {
    uint32_t frame_length; // samples per channel in a full frame
    uint8_t bit_depth;     // only 16 is supported
    uint8_t pb;            // Golomb parameters (the encoder's defaults are 40, 10 and 14)
    uint8_t mb;
    uint8_t kb;
} ALACConfig;

/*
 * Decodes the frame at the start of in. Returns the number of samples per channel written to out (interleaved, at most
 * max_frames per channel; out must hold max_frames * ALAC_MAX_CHANNELS samples) and sets *consumed to the frame's size
 * in bytes and *channels to its channel count. Returns 0 if in does not hold a whole frame yet and a negative value if
 * the data is not a valid frame.
 */
int alac_decode_frame(const ALACConfig *config, const uint8_t *in, size_t len, int16_t *out, unsigned max_frames,
                      size_t *consumed, unsigned *channels);

#endif
