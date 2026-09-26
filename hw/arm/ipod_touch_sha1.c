#include "hw/arm/ipod_touch_sha1.h"
#include "hw/arm/ipod_touch_guard.h"
#include "migration/vmstate.h"

/* XNU hands this engine whole pages; a megabyte is orders of magnitude of
 * headroom, and the point is only to keep a garbage value away from malloc. */
/* Same ceiling as the AES engine. The point is only to keep a garbage
 * value away from g_malloc, so it should sit far above any real transfer
 * rather than as close as possible to one -- a clamp that trips on a legitimate
 * hash would silently corrupt the digest and fail a signature check. */
#define IT_SHA1_MAX_XFER (16 * 1024 * 1024)

/*
 * S5L8720 SHA1 engine.
 *
 * The block is a raw SHA1 compression engine, not a whole-message hasher:
 *
 *   - the guest loads a chaining state into the five hash registers at 0x20
 *     (each register holds one state word in big-endian byte order),
 *   - it feeds whole 64-byte message blocks, either a block at a time through
 *     the hardware buffer at 0x40 or by pointing the engine at physical
 *     memory (memory mode, 0x80/0x84/0x8c),
 *   - the resulting chaining state is read back out of the hash registers.
 *
 * Padding and finalisation belong to the guest. iBoot pre-pads its messages,
 * so for iBoot the state after the last block already is the digest. XNU
 * instead hands whole pages to the engine and then runs its own SHA1Final
 * over the padding block in software (libkern SHA1UpdateUsePhysicalAddress),
 * which only works if the engine returns the intermediate state.
 *
 * Jobs complete inside the config write, but the driver can ask to be told
 * about it: 3.1.3 arms SHA_INTENABLE and then sleeps on its command gate
 * until the completion interrupt fires.
 */

static const uint32_t sha1_iv[5] = {
    0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0
};

static bool sha1_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_SHA1_DEBUG") != NULL;
    }
    return on;
}

bool ipod_touch_sha1_last_hash(IPodTouchSHA1State *s, uint8_t out[20])
{
    if (!s || !s->last_hash_valid) {
        return false;
    }
    memcpy(out, s->last_hash, sizeof(s->last_hash));
    return true;
}

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* One SHA1 compression round over a 64-byte block, in place on state[]. */
static void sha1_compress(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t tmp = ROTL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROTL(b, 30); b = a; a = tmp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

/* Publish the current chaining state for the PKE forge, big-endian. */
static void sha1_publish(IPodTouchSHA1State *s)
{
    for (int i = 0; i < 5; i++) {
        s->last_hash[i * 4 + 0] = s->state[i] >> 24;
        s->last_hash[i * 4 + 1] = s->state[i] >> 16;
        s->last_hash[i * 4 + 2] = s->state[i] >> 8;
        s->last_hash[i * 4 + 3] = s->state[i];
    }
    s->last_hash_valid = true;
}

/* Drop the completion interrupt; every acknowledge path funnels through here. */
static void sha1_clear_irq(IPodTouchSHA1State *s)
{
    if (s->int_status) {
        s->int_status = 0;
        if (s->irq) {
            qemu_irq_lower(s->irq);
        }
    }
}

static void sha1_reset(IPodTouchSHA1State *s)
{
    s->last_hash_valid = false;
    memset(s->last_hash, 0, sizeof(s->last_hash));
	s->config = 0;
	s->memory_start = 0;
	s->memory_mode = 0;
	s->insize = 0;
    memcpy(s->state, sha1_iv, sizeof(s->state));
	memset(&s->hw_buffer, 0, sizeof(s->hw_buffer));
	s->hw_buffer_dirty = false;
    sha1_clear_irq(s);
}

/* Run the blocks the guest has queued through the compression function.
 * Only a start with config bit 2 asks for the completion interrupt. 7E18's
 * AppleS5L8900XSHA1 polls short jobs (start 0x2/0xa) with SHA_INTENABLE still
 * set from an earlier interrupt-driven job, then completes them itself; an
 * interrupt there runs its completion again with no request (NULL+0x28 kernel
 * abort at 0xc06d81c8 while installing apps with many files). */
static void sha1_run(IPodTouchSHA1State *s, bool notify)
{
    if (s->hw_buffer_dirty) {
        sha1_compress(s->state, (const uint8_t *)s->hw_buffer);
        memset(s->hw_buffer, 0, sizeof(s->hw_buffer));
        s->hw_buffer_dirty = false;
    }

    if (s->memory_mode) {
        uint32_t nblocks = s->insize / 0x40;

        /*
         * Fetch the whole region once rather than per 64-byte block. XNU hands
         * this engine entire pages to hash, so a block-at-a-time read was 64
         * full address-space dispatches per page -- all of it on the vCPU
         * thread inside the MMIO handler, and all of it to walk memory that is
         * contiguous anyway.
         */
        if (nblocks) {
            uint8_t *buf = g_malloc(nblocks * 0x40);

            cpu_physical_memory_read(s->memory_start, buf, nblocks * 0x40);
            for (uint32_t i = 0; i < nblocks; i++) {
                sha1_compress(s->state, buf + i * 0x40);
            }
            g_free(buf);
        }
    }

    sha1_publish(s);

    if (sha1_trace()) {
        printf("[SHA1] state=");
        for (int i = 0; i < 20; i++) {
            printf("%02x", s->last_hash[i]);
        }
        printf("\n");
        fflush(stdout);
    }

    if (notify && s->int_enable) {
        s->int_status = 1;
        if (s->irq) {
            qemu_irq_raise(s->irq);
        }
    }
}

static uint64_t ipod_touch_sha1_read(void *opaque, hwaddr offset, unsigned size)
{
	IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

    if (sha1_trace() && (offset < SHA_HASHOUT || offset > SHA_HASHOUT_END)) {
        printf("[SHA1] RD %#04x\n", (unsigned)offset);
        fflush(stdout);
    }

	switch(offset) {
		case SHA_CONFIG:
			return s->config;
		case SHA_RESET:
			return 0;
		case SHA_INTSTATUS:
			return s->int_status;
		case SHA_INTENABLE:
			return s->int_enable;
		case SHA_MEMORY_START:
			return s->memory_start;
		case SHA_MEMORY_MODE:
			return s->memory_mode;
		case SHA_INSIZE:
			return s->insize;
		case SHA_HASHOUT ... SHA_HASHOUT_END:
			/* Big-endian state word, matching what the guest writes in. */
			return bswap32(s->state[(offset - SHA_HASHOUT) / 4]);
	}

    return 0;
}

static void ipod_touch_sha1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

    if (sha1_trace() && (offset < SHA_HWBUF || offset > SHA_HWBUF_END)) {
        printf("[SHA1] WR %#04x <- %#010llx\n", (unsigned)offset,
               (unsigned long long)value);
        fflush(stdout);
    }

	switch(offset) {
		case SHA_CONFIG:
			/*
			 * Bit 1 is the start bit. 2.1.1 uses 0x2/0xa; 3.1.3 adds bit 2
			 * once it goes interrupt-driven (0xe), so match on the bit rather
			 * than on whole values.
			 */
			if (value & 0x2)
			{
                if (sha1_trace()) {
                    printf("[SHA1] GO config=%#llx memory_mode=%u start=%#x "
                           "insize=%#x hw_dirty=%d int_en=%u\n",
                           (unsigned long long)value,
                           s->memory_mode, s->memory_start, s->insize,
                           (int)s->hw_buffer_dirty, s->int_enable);
                    fflush(stdout);
                }
                sha1_clear_irq(s);
                sha1_run(s, value & 0x4);
			} else {
				s->config = value;
			}
			break;
		case SHA_RESET:
			sha1_reset(s);
			break;
		case SHA_INTSTATUS:
			/* Any write acknowledges the completion interrupt. */
			sha1_clear_irq(s);
			break;
		case SHA_INTENABLE:
			s->int_enable = value;
			if (!value) {
				sha1_clear_irq(s);
			}
			break;
		case SHA_MEMORY_START:
			s->memory_start = value;
			break;
		case SHA_MEMORY_MODE:
			s->memory_mode = value;
			break;
		case SHA_INSIZE:
			/* Bounded like every other engine's length (ipod_touch_aes.c:510).
			 * Raw, this value reaches g_malloc and cpu_physical_memory_read
			 * directly: 0xFFFFFFFF asks for a 4 GiB allocation on the vCPU
			 * thread inside the MMIO handler, and g_malloc ABORTS the process
			 * when it fails -- one guest store takes the user's whole session
			 * down with it. */
			s->insize = IT_SIZE("sha1", value, IT_SHA1_MAX_XFER);
			break;
		case SHA_HASHOUT ... SHA_HASHOUT_END:
			/* Load the chaining state to continue from. */
			s->state[(offset - SHA_HASHOUT) / 4] = bswap32((uint32_t)value);
			break;
		case SHA_HWBUF ... SHA_HWBUF_END:
			s->hw_buffer[(offset - SHA_HWBUF) / 4] = value;
			s->hw_buffer_dirty = true;
			break;
	}
}

static const MemoryRegionOps sha1_ops = {
    .read = ipod_touch_sha1_read,
    .write = ipod_touch_sha1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_sha1_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchSHA1State *s = IPOD_TOUCH_SHA1(dev);

    memory_region_init_io(&s->iomem, obj, &sha1_ops, s, "sha1", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    sha1_reset(s);
}

/* The engine chains from whatever the guest last left in the hash registers,
 * so a stale chaining state would corrupt the first digest of a second boot.
 * int_status must go with the IRQ line, not just be zeroed alongside it. */
static void ipod_touch_sha1_reset(DeviceState *dev)
{
    IPodTouchSHA1State *s = IPOD_TOUCH_SHA1(dev);

    s->last_hash_valid = false;
    memset(s->last_hash, 0, sizeof(s->last_hash));
    s->config = 0;
    s->memory_start = 0;
    s->memory_mode = 0;
    s->insize = 0;
    memset(s->state, 0, sizeof(s->state));
    memset(s->hw_buffer, 0, sizeof(s->hw_buffer));
    s->hw_buffer_dirty = false;
    s->int_enable = 0;
    s->int_status = 0;
    if (s->irq) {
        qemu_irq_lower(s->irq);
    }
}

/* The chaining state in state[] is the whole point: a digest in progress at
 * snapshot time must resume from exactly the same intermediate value. */
static const VMStateDescription vmstate_ipod_touch_sha1 = {
    .name = "ipod_touch_sha1",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(last_hash, IPodTouchSHA1State, 20),
        VMSTATE_BOOL(last_hash_valid, IPodTouchSHA1State),
        VMSTATE_UINT32(config, IPodTouchSHA1State),
        VMSTATE_UINT32(memory_start, IPodTouchSHA1State),
        VMSTATE_UINT32(memory_mode, IPodTouchSHA1State),
        VMSTATE_UINT32(insize, IPodTouchSHA1State),
        VMSTATE_UINT32_ARRAY(state, IPodTouchSHA1State, 5),
        VMSTATE_UINT32_ARRAY(hw_buffer, IPodTouchSHA1State, 0x10),
        VMSTATE_BOOL(hw_buffer_dirty, IPodTouchSHA1State),
        VMSTATE_UINT32(int_enable, IPodTouchSHA1State),
        VMSTATE_UINT32(int_status, IPodTouchSHA1State),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_sha1_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ipod_touch_sha1_reset);
    dc->vmsd = &vmstate_ipod_touch_sha1;
}

static const TypeInfo ipod_touch_sha1_info = {
    .name          = TYPE_IPOD_TOUCH_SHA1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchSHA1State),
    .instance_init = ipod_touch_sha1_init,
    .class_init    = ipod_touch_sha1_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_sha1_info);
}

type_init(ipod_touch_machine_types)
