#include "hw/arm/ipod_touch_sha1.h"

/*
 * The guest pads the message itself (0x80, zeroes, 64-bit bit length), so the engine only runs the SHA-1 compression
 * function over 64-byte blocks. We feed every block to OpenSSL as it arrives; after the padded final block the
 * context state h0..h4 is the digest, so there is no limit on the input size (the restore ramdisk is ~25 MB).
 */
static void sha1_process(IPodTouchSHA1State *s, const void *data, size_t len)
{
    SHA1_Update(&s->ctx, data, len);
    s->hash_computed = false;
}

static void flush_hw_buffer(IPodTouchSHA1State *s) {
    sha1_process(s, s->hw_buffer, 0x40);
    memset(s->hw_buffer, 0, 0x40);
    s->hw_buffer_dirty = false;
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
	SHA1_Init(&s->ctx);
}

static uint64_t ipod_touch_sha1_read(void *opaque, hwaddr offset, unsigned size)
{
	IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

	switch(offset) {
		case SHA_CONFIG:
			return s->config;
		case SHA_RESET:
			return 0;
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
                const SHA_LONG h[5] = { s->ctx.h0, s->ctx.h1, s->ctx.h2, s->ctx.h3, s->ctx.h4 };
                for(int i = 0; i < 5; i++) {
                    stl_be_p(&s->hashout[i * 4], h[i]);
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
			if(value == 0x2 || value == 0xa)
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
						cpu_physical_memory_read(s->memory_start + done, chunk, n);
						sha1_process(s, chunk, n);
					}
				}
			} else {
				s->config = value;
			}
			break;
		case SHA_RESET:
			sha1_reset(s);
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

    sha1_reset(s);
}

static void ipod_touch_sha1_class_init(ObjectClass *klass, void *data)
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
