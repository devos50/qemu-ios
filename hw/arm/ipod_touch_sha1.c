#include "hw/arm/ipod_touch_sha1.h"
#include "system/physmem.h"
#include "qemu/bitops.h"

/*
 * The guest pads the message itself (0x80, zeroes, 64-bit bit length), so the engine only runs the SHA-1 compression
 * function over 64-byte blocks. We compress every block as it arrives; after the padded final block the state
 * h0..h4 is the digest, so there is no limit on the input size (the restore ramdisk is ~25 MB).
 *
 * The bootloaders poll the engine. The kernel's AppleS5L8900XSHA1 (used through /dev/sha1_0, e.g. by asr from iPhone OS
 * 2.2 on) hashes a request in chunks: it resets the engine, writes the state of the previous chunk back into the hash
 * registers and starts with SHA_CONFIG_CONTINUE, and it waits for an interrupt for chunks of 4 KiB and more.
 */
static void sha1_compress(uint32_t state[5], const uint8_t *block)
{
    uint32_t w[80];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

    for(int i = 0; i < 16; i++) {
        w[i] = ldl_be_p(block + i * 4);
    }
    for(int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for(int i = 0; i < 80; i++) {
        uint32_t f, k;
        if(i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if(i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if(i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_process(IPodTouchSHA1State *s, const uint8_t *data, size_t len)
{
    // the engine only takes whole blocks: hardware buffer flushes and memory mode lengths are multiples of 64
    for(size_t off = 0; off + 64 <= len; off += 64) {
        sha1_compress(s->state, data + off);
    }
    s->hash_computed = false;
}

static void flush_hw_buffer(IPodTouchSHA1State *s) {
    sha1_process(s, (const uint8_t *)s->hw_buffer, 0x40);
    memset(s->hw_buffer, 0, 0x40);
    s->hw_buffer_dirty = false;
}

static void sha1_update_irq(IPodTouchSHA1State *s)
{
    qemu_set_irq(s->irq, s->irq_status && s->irq_enable);
}

static void sha1_reset(IPodTouchSHA1State *s)
{
	s->config = 0;
	s->memory_start = 0;
	s->memory_mode = 0;
	s->insize = 0;
	memset(&s->hw_buffer, 0, 0x10 * sizeof(uint32_t));
	memset(&s->hashout, 0, 0x14);
	s->hw_buffer_dirty = false;
	s->hash_computed = false;
	s->state[0] = 0x67452301;
	s->state[1] = 0xefcdab89;
	s->state[2] = 0x98badcfe;
	s->state[3] = 0x10325476;
	s->state[4] = 0xc3d2e1f0;
}

static uint64_t ipod_touch_sha1_read(void *opaque, hwaddr offset, unsigned size)
{
	IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

	switch(offset) {
		case SHA_CONFIG:
			return s->config & ~SHA_CONFIG_BUSY; // operations complete immediately
		case SHA_RESET:
			return 0;
		case SHA_IRQ_STATUS:
			return s->irq_status;
		case SHA_IRQ_ENABLE:
			return s->irq_enable;
		case SHA_MEMORY_START:
			return s->memory_start;
		case SHA_MEMORY_MODE:
			return s->memory_mode;
		case SHA_INSIZE:
			return s->insize;
		/* Hash result ouput */
		case 0x20 ... 0x34:
            if(!s->hash_computed) {
                // the digest is the big-endian state after the guest-padded final block
                for(int i = 0; i < 5; i++) {
                    stl_be_p(&s->hashout[i * 4], s->state[i]);
                }
                s->hash_computed = true;
            }

			return ldl_le_p(&s->hashout[offset - 0x20]);
	}

    return 0;
}

static void ipod_touch_sha1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

	switch(offset) {
		case SHA_CONFIG:
			if(value & SHA_CONFIG_START)
			{
                if(s->hw_buffer_dirty) {
                    flush_hw_buffer(s);
                }

				if(s->memory_mode)
				{
					// we are in memory mode - hash the input straight from memory, a chunk at a time
					uint8_t chunk[0x1000];
					uint32_t len = s->insize & ~0x3f;
					for(uint32_t done = 0; done < len; done += sizeof(chunk)) {
						uint32_t n = MIN(len - done, sizeof(chunk));
						physical_memory_read(s->memory_start + done, chunk, n);
						sha1_process(s, chunk, n);
					}
				}

				if(value & SHA_CONFIG_IRQ) {
					s->irq_status = 1;
					sha1_update_irq(s);
				}
			} else {
				s->config = value;
			}
			break;
		case SHA_RESET:
			sha1_reset(s);
			break;
		case SHA_IRQ_STATUS:
			if(value & 1) {
				s->irq_status = 0;
				sha1_update_irq(s);
			}
			break;
		case SHA_IRQ_ENABLE:
			s->irq_enable = value & 1;
			sha1_update_irq(s);
			break;
		case SHA_HASH ... SHA_HASH + 0x10:
			// the state to continue from (the kernel hashes long inputs in chunks), in the byte order the registers
			// read back
			s->state[(offset - SHA_HASH) / 4] = bswap32(value);
			s->hash_computed = false;
			break;
		case SHA_MEMORY_START:
			s->memory_start = value;
			break;
		case SHA_MEMORY_MODE:
			s->memory_mode = value;
			break;
		case SHA_INSIZE:
			s->insize = value;
			break;
		case 0x40 ... 0x7c:
            // write to the hardware buffer
            s->hw_buffer[(offset - 0x40) / 4] |= value;
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

static void ipod_touch_sha1_class_init(ObjectClass *klass, const void *data)
{

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
