/*
 * Apple Lossless (ALAC) frame decoder for the AMC model, 16-bit only.
 *
 * Follows the structure of Apple's reference decoder (ALACDecoder.cpp, ag_dec.c, dp_dec.c, matrix_dec.c): a frame is
 * a sequence of elements, each starting with a 3-bit tag, and ends with ID_END. Audio elements hold adaptive Golomb
 * coded prediction residuals, an adaptive LPC filter per channel and, for stereo, mid/side mixing parameters.
 */
#include "hw/arm/ipod_touch_alac.h"

enum {
    ID_SCE = 0, // single channel element
    ID_CPE = 1, // channel pair element
    ID_CCE = 2,
    ID_LFE = 3,
    ID_DSE = 4, // data stream element
    ID_PCE = 5,
    ID_FIL = 6, // fill element
    ID_END = 7,
};

#define QBSHIFT 9
#define QB (1u << QBSHIFT)
#define MMULSHIFT 2
#define MDENSHIFT (QBSHIFT - MMULSHIFT - 1)
#define MOFF (1u << (MDENSHIFT - 2))
#define BITOFF 24
#define MAX_PREFIX_16 9
#define MAX_PREFIX_32 9
#define MAX_DATATYPE_BITS_16 16
#define N_MAX_MEAN_CLAMP 0xffff
#define N_MEAN_CLAMP_VAL 0xffff
#define MAX_COEFS 32

typedef struct BitReader {
    const uint8_t *buf;
    size_t len;
    uint64_t pos; // in bits
} BitReader;

/* The 32 bits at bit position pos; bits past the end of the data read as zero. */
static uint32_t peek32(const BitReader *b, uint64_t pos)
{
    uint64_t v = 0;
    size_t byte = pos >> 3;

    for (int i = 0; i < 5; i++) {
        v = (v << 8) | (byte + i < b->len ? b->buf[byte + i] : 0);
    }
    return (uint32_t)(v >> (8 - (pos & 7)));
}

static uint32_t read_bits(BitReader *b, unsigned n)
{
    uint32_t v;

    if (n == 0) {
        return 0;
    }
    v = peek32(b, b->pos) >> (32 - n);
    b->pos += n;
    return v;
}

static int32_t read_signed(BitReader *b, unsigned n)
{
    uint32_t v = read_bits(b, n);
    unsigned shift = 32 - n;
    return (int32_t)(v << shift) >> shift;
}

static bool overrun(const BitReader *b)
{
    return b->pos > (uint64_t)b->len * 8;
}

static unsigned lead(uint32_t x)
{
    return x ? __builtin_clz(x) : 32;
}

static int32_t sign_of(int32_t i)
{
    return (i > 0) - (i < 0);
}

/* A Golomb-coded value with an escape to 16 raw bits, for run lengths. */
static uint32_t dyn_get(BitReader *b, uint32_t m, uint32_t k)
{
    uint32_t stream = peek32(b, b->pos);
    uint32_t pre = lead(~stream);

    if (pre >= MAX_PREFIX_16) {
        b->pos += MAX_PREFIX_16;
        return read_bits(b, MAX_DATATYPE_BITS_16);
    }
    uint32_t v = k ? (uint32_t)((uint64_t)stream << (pre + 1)) >> (32 - k) : 0;
    uint32_t result = pre * m + v - 1;
    b->pos += pre + 1 + k;
    if (v < 2) {
        result -= v - 1;
        b->pos -= 1;
    }
    return result;
}

/* A Golomb-coded value with an escape to max_bits raw bits, for residuals. */
static uint32_t dyn_get_32bit(BitReader *b, uint32_t m, uint32_t k, unsigned max_bits)
{
    uint32_t stream = peek32(b, b->pos);
    uint32_t result = lead(~stream);

    if (result >= MAX_PREFIX_32) {
        b->pos += MAX_PREFIX_32;
        return read_bits(b, max_bits);
    }
    uint32_t v = k ? (uint32_t)((uint64_t)stream << (result + 1)) >> (32 - k) : 0;
    b->pos += result + 1 + k;
    result *= m;
    if (v >= 2) {
        result += v - 1;
    } else {
        b->pos -= 1;
    }
    return result;
}

/* Adaptive Golomb decoding of num prediction residuals (ag_dec.c dyn_decomp). */
static int dyn_decomp(const ALACConfig *config, unsigned pb, BitReader *b, int32_t *out, unsigned num,
                      unsigned max_size)
{
    uint32_t mb = config->mb, kb = config->kb, wb = (1u << kb) - 1;
    uint32_t zero = 0;
    unsigned c = 0;

    while (c < num) {
        uint32_t m = mb >> QBSHIFT;
        uint32_t k = MIN(31 - lead(m + 3), kb);
        uint32_t n;

        m = (1u << k) - 1;
        n = dyn_get_32bit(b, m, k, max_size);
        {
            uint32_t ndecode = n + zero;
            int32_t multiplier = -(int32_t)(ndecode & 1);
            multiplier |= 1;
            out[c++] = (int32_t)((ndecode + 1) >> 1) * multiplier;
        }
        mb = pb * (n + zero) + mb - ((pb * mb) >> QBSHIFT);
        if (n > N_MAX_MEAN_CLAMP) {
            mb = N_MEAN_CLAMP_VAL;
        }
        zero = 0;

        if (((mb << MMULSHIFT) < QB) && (c < num)) {
            zero = 1;
            k = lead(mb) - BITOFF + ((mb + MOFF) >> MDENSHIFT);
            uint32_t mz = ((1u << k) - 1) & wb;
            n = dyn_get(b, mz, k);
            if (c + n > num) {
                return -1;
            }
            for (uint32_t j = 0; j < n; j++) {
                out[c++] = 0;
            }
            if (n >= 65535) {
                zero = 0;
            }
            mb = 0;
        }
        if (overrun(b)) {
            return -1;
        }
    }
    return 0;
}

/* Undoes the adaptive LPC prediction (dp_dec.c unpc_block); in and out may be the same buffer. */
static void unpc_block(const int32_t *in, int32_t *out, unsigned num, int16_t *coefs, unsigned numactive,
                       unsigned chanbits, unsigned denshift)
{
    unsigned chanshift = 32 - chanbits;
    int32_t denhalf = denshift ? 1 << (denshift - 1) : 0;

    out[0] = in[0];
    if (numactive == 0) {
        if (num > 1 && in != out) {
            memcpy(&out[1], &in[1], (num - 1) * sizeof(int32_t));
        }
        return;
    }
    if (numactive == 31) {
        int32_t prev = out[0];
        for (unsigned j = 1; j < num; j++) {
            int32_t del = in[j] + prev;
            prev = (int32_t)((uint32_t)del << chanshift) >> chanshift;
            out[j] = prev;
        }
        return;
    }

    for (unsigned j = 1; j <= numactive && j < num; j++) {
        int32_t del = in[j] + out[j - 1];
        out[j] = (int32_t)((uint32_t)del << chanshift) >> chanshift;
    }

    unsigned lim = numactive + 1;
    for (unsigned j = lim; j < num; j++) {
        int32_t sum1 = 0;
        const int32_t *pout = out + j - 1;
        int32_t top = out[j - lim];

        for (unsigned k = 0; k < numactive; k++) {
            sum1 += coefs[k] * (pout[-(int)k] - top);
        }
        int32_t del = in[j];
        int32_t del0 = del;
        int32_t sg = sign_of(del);
        del += top + ((sum1 + denhalf) >> denshift);
        out[j] = (int32_t)((uint32_t)del << chanshift) >> chanshift;

        if (sg > 0) {
            for (int k = numactive - 1; k >= 0; k--) {
                int32_t dd = top - pout[-k];
                int32_t sgn = sign_of(dd);
                coefs[k] -= sgn;
                del0 -= (numactive - k) * ((sgn * dd) >> denshift);
                if (del0 <= 0) {
                    break;
                }
            }
        } else if (sg < 0) {
            for (int k = numactive - 1; k >= 0; k--) {
                int32_t dd = top - pout[-k];
                int32_t sgn = sign_of(dd);
                coefs[k] += sgn;
                del0 -= (numactive - k) * ((-sgn * dd) >> denshift);
                if (del0 >= 0) {
                    break;
                }
            }
        }
    }
}

typedef struct ChannelParams {
    unsigned mode;
    unsigned denshift;
    unsigned pb_factor;
    unsigned num;
    int16_t coefs[MAX_COEFS];
} ChannelParams;

static void read_channel_params(BitReader *b, ChannelParams *p)
{
    uint32_t header = read_bits(b, 8);
    p->mode = header >> 4;
    p->denshift = header & 0xf;
    header = read_bits(b, 8);
    p->pb_factor = header >> 5;
    p->num = header & 0x1f;
    for (unsigned i = 0; i < p->num; i++) {
        p->coefs[i] = (int16_t)read_bits(b, 16);
    }
}

static int decode_channel(const ALACConfig *config, BitReader *b, ChannelParams *p, int32_t *predictor,
                          int32_t *mix, unsigned num, unsigned chanbits)
{
    if (dyn_decomp(config, (config->pb * p->pb_factor) / 4, b, predictor, num, chanbits) < 0) {
        return -1;
    }
    if (p->mode == 0) {
        unpc_block(predictor, mix, num, p->coefs, p->num, chanbits, p->denshift);
    } else {
        // the stream was predicted twice
        unpc_block(predictor, predictor, num, NULL, 31, chanbits, 0);
        unpc_block(predictor, mix, num, p->coefs, p->num, chanbits, p->denshift);
    }
    return 0;
}

int alac_decode_frame(const ALACConfig *config, const uint8_t *in, size_t len, int16_t *out, unsigned max_frames,
                      size_t *consumed, unsigned *channels)
{
    static int32_t predictor[ALAC_MAX_FRAME_LENGTH];
    static int32_t mix_u[ALAC_MAX_FRAME_LENGTH], mix_v[ALAC_MAX_FRAME_LENGTH];
    BitReader b = { .buf = in, .len = len };
    unsigned out_channels = 0, frames = 0;

    if (config->bit_depth != 16 || config->frame_length > ALAC_MAX_FRAME_LENGTH) {
        return -1;
    }

    for (;;) {
        unsigned tag = read_bits(&b, 3);

        if (overrun(&b)) {
            return 0;
        }
        if (tag == ID_END) {
            break;
        }
        if (tag == ID_DSE) {
            read_bits(&b, 4); // element instance tag
            bool align = read_bits(&b, 1);
            unsigned count = read_bits(&b, 8);
            if (count == 255) {
                count += read_bits(&b, 8);
            }
            if (align) {
                b.pos = (b.pos + 7) & ~7ull;
            }
            b.pos += count * 8;
            continue;
        }
        if (tag == ID_FIL) {
            unsigned count = read_bits(&b, 4);
            if (count == 15) {
                count += read_bits(&b, 8) - 1;
            }
            b.pos += count * 8;
            continue;
        }
        if (tag != ID_SCE && tag != ID_CPE) {
            return -1;
        }

        unsigned element_channels = tag == ID_CPE ? 2 : 1;
        if (out_channels + element_channels > ALAC_MAX_CHANNELS) {
            return -1;
        }
        read_bits(&b, 4); // element instance tag
        if (read_bits(&b, 12) != 0) {
            return -1;
        }
        uint32_t header = read_bits(&b, 4);
        bool partial = header >> 3;
        unsigned bytes_shifted = (header >> 1) & 3;
        bool escape = header & 1;
        unsigned num = config->frame_length;

        if (bytes_shifted != 0) {
            return -1; // only used for more than 16 bits per sample
        }
        if (partial) {
            num = read_bits(&b, 16) << 16;
            num |= read_bits(&b, 16);
        }
        if (num == 0 || num > ALAC_MAX_FRAME_LENGTH || num > max_frames || (frames && num != frames)) {
            return -1;
        }
        frames = num;

        unsigned mix_bits = 0;
        int mix_res = 0;
        if (!escape) {
            ChannelParams u, v;
            unsigned chanbits = config->bit_depth + (element_channels - 1);

            mix_bits = read_bits(&b, 8);
            mix_res = (int8_t)read_bits(&b, 8);
            read_channel_params(&b, &u);
            if (element_channels == 2) {
                read_channel_params(&b, &v);
            }
            if (overrun(&b) || decode_channel(config, &b, &u, predictor, mix_u, num, chanbits) < 0 ||
                (element_channels == 2 && decode_channel(config, &b, &v, predictor, mix_v, num, chanbits) < 0)) {
                return overrun(&b) ? 0 : -1;
            }
        } else {
            for (unsigned i = 0; i < num; i++) {
                mix_u[i] = read_signed(&b, config->bit_depth);
                if (element_channels == 2) {
                    mix_v[i] = read_signed(&b, config->bit_depth);
                }
            }
        }

        // matrix_dec.c unmix16
        for (unsigned i = 0; i < num; i++) {
            int16_t *o = &out[i * ALAC_MAX_CHANNELS + out_channels];
            if (element_channels == 1) {
                o[0] = mix_u[i];
            } else if (mix_res != 0) {
                int32_t l = mix_u[i] + mix_v[i] - ((mix_res * mix_v[i]) >> mix_bits);
                o[0] = l;
                o[1] = l - mix_v[i];
            } else {
                o[0] = mix_u[i];
                o[1] = mix_v[i];
            }
        }
        out_channels += element_channels;
    }

    if (overrun(&b) || out_channels == 0) {
        return overrun(&b) ? 0 : -1;
    }
    // the samples were written with a stride of ALAC_MAX_CHANNELS: pack them for a mono stream
    if (out_channels == 1) {
        for (unsigned i = 1; i < frames; i++) {
            out[i] = out[i * ALAC_MAX_CHANNELS];
        }
    }
    *consumed = (b.pos + 7) / 8;
    *channels = out_channels;
    return frames;
}
