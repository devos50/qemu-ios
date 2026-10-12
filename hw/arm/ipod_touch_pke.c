#include "hw/arm/ipod_touch_pke.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"

/*
 * Modular arithmetic on little-endian numbers of n 32-bit limbs, enough for the RSA signature check the engine is
 * used for: base^65537 mod modulus with keys of up to 2048 bits. Multiplication is shift-and-add, which takes a few
 * milliseconds for 17 multiplications of 2048-bit numbers.
 */
#define PKE_MAX_LIMBS (sizeof(((IPodTouchPKEState *)0)->segments) / 4)
#define PKE_EXPONENT  65537

static int pke_cmp(const uint32_t *a, const uint32_t *b, int n)
{
    for(int i = n - 1; i >= 0; i--) {
        if(a[i] != b[i]) {
            return a[i] > b[i] ? 1 : -1;
        }
    }
    return 0;
}

static void pke_sub(uint32_t *r, const uint32_t *a, int n)
{
    uint64_t borrow = 0;
    for(int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)r[i] - a[i] - borrow;
        r[i] = d;
        borrow = (d >> 32) & 1;
    }
}

/* r = (r + a) mod m, for r, a < m */
static void pke_add_mod(uint32_t *r, const uint32_t *a, const uint32_t *m, int n)
{
    uint64_t carry = 0;
    for(int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)r[i] + a[i] + carry;
        r[i] = t;
        carry = t >> 32;
    }
    // the sum is below 2m, and subtracting m also undoes a carry out of the top limb
    if(carry || pke_cmp(r, m, n) >= 0) {
        pke_sub(r, m, n);
    }
}

/* r = (a * b) mod m, for a < m */
static void pke_mul_mod(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, int n)
{
    uint32_t acc[PKE_MAX_LIMBS] = { 0 };

    for(int bit = n * 32 - 1; bit >= 0; bit--) {
        pke_add_mod(acc, acc, m, n);
        if(b[bit / 32] & (1u << (bit % 32))) {
            pke_add_mod(acc, a, m, n);
        }
    }
    memcpy(r, acc, n * 4);
}

/* r = base^PKE_EXPONENT mod m */
static void pke_exp_mod(uint32_t *r, const uint32_t *base, const uint32_t *m, int n)
{
    uint32_t one[PKE_MAX_LIMBS] = { 1 };
    uint32_t b[PKE_MAX_LIMBS];
    uint32_t acc[PKE_MAX_LIMBS];

    pke_mul_mod(b, one, base, m, n); // reduce the base below m
    memcpy(acc, b, n * 4);
    for(int bit = 30 - clz32(PKE_EXPONENT); bit >= 0; bit--) {
        pke_mul_mod(acc, acc, acc, m, n);
        if(PKE_EXPONENT & (1u << bit)) {
            pke_mul_mod(acc, acc, b, m, n);
        }
    }
    memcpy(r, acc, n * 4);
}

static uint64_t ipod_touch_pke_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchPKEState *s = (IPodTouchPKEState *)opaque;

    //printf("%s: offset 0x%08x\n", __FUNCTION__, offset);

    switch(offset) {
        case REG_PKE_SEG_SIZE:
            return s->seg_size_reg;
        case REG_PKE_SEG_START ... (REG_PKE_SEG_START + 1024):
        {
            uint32_t *res = (uint32_t *)s->segments;
            return res[(offset - REG_PKE_SEG_START) / 4];
        }
        default:
            break;
    }

    return 0;
}

static void ipod_touch_pke_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchPKEState *s = (IPodTouchPKEState *)opaque;

    //printf("%s: offset 0x%08x value 0x%08x\n", __FUNCTION__, offset, value);

    switch(offset) {
        case 0x0:
            s->num_started = 0;
            break;
        case 0x10:
            break;
        case REG_PKE_SEG_START ... (REG_PKE_SEG_START + 1024):
        {
            uint32_t *segments_cast = (uint32_t *)s->segments;
            segments_cast[(offset - REG_PKE_SEG_START) / 4] = value;
            break;
        }
        case REG_PKE_START:
        {
            s->num_started++;

            if(s->num_started == 5) { // TODO this is arbitrary!
                // SEG0 holds the modulus and SEG1 the base; the result replaces the base
                int n = s->segment_size / 4;
                uint32_t mod[PKE_MAX_LIMBS], base[PKE_MAX_LIMBS], res[PKE_MAX_LIMBS];
                if(n == 0) {
                    break;
                }
                for(int i = 0; i < n; i++) {
                    mod[i] = ldl_le_p(s->segments + i * 4);
                    base[i] = ldl_le_p(s->segments + s->segment_size + i * 4);
                }
                pke_exp_mod(res, base, mod, n);
                for(int i = 0; i < n; i++) {
                    stl_le_p(s->segments + s->segment_size + i * 4, res[i]);
                }
            }
            break;
        }
        case REG_PKE_SEG_SIZE:
            s->seg_size_reg = value;
            uint32_t size_bit = (s->seg_size_reg >> 6);
            if(size_bit == 0) { s->segment_size = 256; }
            else if(size_bit == 1) { s->segment_size = 128; }
            else { }
            break;
        case REG_PKE_SWRESET:
            s->num_started = 0;
            break;
        default:
            break;
    }
}

static const MemoryRegionOps pke_ops = {
    .read = ipod_touch_pke_read,
    .write = ipod_touch_pke_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_pke_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchPKEState *s = IPOD_TOUCH_PKE(dev);

    memory_region_init_io(&s->iomem, obj, &pke_ops, s, "pke", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void ipod_touch_pke_class_init(ObjectClass *klass, const void *data)
{

}

static const TypeInfo ipod_touch_pke_info = {
    .name          = TYPE_IPOD_TOUCH_PKE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchPKEState),
    .instance_init = ipod_touch_pke_init,
    .class_init    = ipod_touch_pke_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_pke_info);
}

type_init(ipod_touch_machine_types)