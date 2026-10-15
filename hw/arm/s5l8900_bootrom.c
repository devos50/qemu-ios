/*
 * The S5L8900's bootrom services, for a machine started without the bootrom.
 *
 * iBoot 1.x still calls into the bootrom: for its USB stack and for the 8900 engine, which checks and decrypts the
 * 8900 containers iBoot loads (the kernelcache among them). It calls the entries of a jump table at the start of the
 * bootrom directly. This device puts a jump table of its own there, written here and not taken from the bootrom:
 * every entry passes r0-r2 and its offset to the device's registers and returns what the device answers in r0. The
 * device does the work of the 8900 engine's entries and lets the USB entries do nothing, which iBoot's boot path
 * doesn't notice (its recovery mode uses its own USB driver). Any other entry is logged with -d unimp.
 *
 * iBoot also copies the 64 KB of the bootrom to RAM and checks its CRC against the bootroms it knows how to patch;
 * this one is none of them, so iBoot leaves it alone.
 *
 * The ipod_touch_1g branch of qemu-ios did the same for the two 8900 entries by patching the real bootrom's
 * routine table, and used the bootrom for the rest.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "crypto/aes.h"
#include "hw/core/cpu.h"
#include "hw/arm/s5l8900_bootrom.h"
#include "migration/vmstate.h"

#define R_ARG0      0x00
#define R_ARG1      0x04
#define R_ARG2      0x08
#define R_CALL      0x0c        // the entry's offset; writing it makes the call
#define R_RESULT    0x10

// where the code goes: the jump table, a stub per entry that loads its offset, and the call through the registers
#define STUBS       0x100
#define STUB_SIZE   8
#define CALL        (STUBS + S5L8900_BOOTROM_ENTRIES * STUB_SIZE)

#define ARM_B(from, to)         (0xea000000 | ((((to) - (from) - 8) >> 2) & 0xffffff))
#define ARM_MOV_R3(imm)         (0xe3a03000 | (imm))
#define ARM_LDR_IP_PC(off)      (0xe59fc000 | (off))
#define ARM_STMIA_IP_R0_R3      0xe88c000f
#define ARM_LDR_R0_IP(off)      (0xe59c0000 | (off))
#define ARM_BX_LR               0xe12fff1e

const uint8_t s5l8900_key_0x837[16] = {
    0x18, 0x84, 0x58, 0xa6, 0xd1, 0x50, 0x34, 0xdf, 0xe3, 0x86, 0xf2, 0x3b, 0x61, 0xd4, 0x37, 0x74,
};

void s5l8900_aes_cbc(const uint8_t *key, const uint8_t *iv_in, uint8_t *buf, size_t len, bool encrypt)
{
    uint8_t iv[AES_BLOCK_SIZE], block[AES_BLOCK_SIZE];
    AES_KEY aes;

    memcpy(iv, iv_in, sizeof(iv));
    if (encrypt) {
        AES_set_encrypt_key(key, 128, &aes);
    } else {
        AES_set_decrypt_key(key, 128, &aes);
    }
    for (size_t off = 0; off + AES_BLOCK_SIZE <= len; off += AES_BLOCK_SIZE) {
        if (encrypt) {
            for (int i = 0; i < AES_BLOCK_SIZE; i++) {
                block[i] = buf[off + i] ^ iv[i];
            }
            AES_encrypt(block, buf + off, &aes);
            memcpy(iv, buf + off, AES_BLOCK_SIZE);
        } else {
            memcpy(block, buf + off, AES_BLOCK_SIZE);
            AES_decrypt(block, buf + off, &aes);
            for (int i = 0; i < AES_BLOCK_SIZE; i++) {
                buf[off + i] ^= iv[i];
            }
            memcpy(iv, block, AES_BLOCK_SIZE);
        }
    }
}

/* The guest passes virtual addresses; they go through the MMU as the CPU sees them. */
static bool rom_guest_rw(vaddr addr, void *buf, size_t len, bool write)
{
    return current_cpu && cpu_memory_rw_debug(current_cpu, addr, buf, len, write) == 0;
}

static bool rom_read_8900_header(vaddr addr, uint8_t *header)
{
    if (!rom_guest_rw(addr, header, S5L8900_8900_HEADER, false)) {
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8900.bootrom: no 8900 header at 0x%" VADDR_PRIx "\n", addr);
        return false;
    }
    return ldl_le_p(header) == S5L8900_8900_MAGIC;
}

/* Like the bootrom without its signature checks: a container is good when it says it is an 8900 container. */
static uint32_t rom_8900_check(S5L8900BootromState *s)
{
    g_autofree uint8_t *header = g_malloc(S5L8900_8900_HEADER);

    return rom_read_8900_header(s->args[0], header);
}

static uint32_t rom_8900_decrypt(S5L8900BootromState *s)
{
    g_autofree uint8_t *header = g_malloc(S5L8900_8900_HEADER);

    if (!rom_read_8900_header(s->args[0], header)) {
        return 0;
    }
    if (header[7] != S5L8900_8900_ENCRYPTED) {
        return 1;
    }
    uint32_t len = ldl_le_p(header + 0xc);
    g_autofree uint8_t *data = g_try_malloc(len);
    if (!data || !rom_guest_rw(s->args[1], data, len, false)) {
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8900.bootrom: can't read %u bytes of 8900 data at 0x%x\n", len,
                      s->args[1]);
        return 0;
    }
    static const uint8_t zero_iv[AES_BLOCK_SIZE];
    s5l8900_aes_cbc(s5l8900_key_0x837, zero_iv, data, len, false);
    return rom_guest_rw(s->args[1], data, len, true);
}

static void rom_call(S5L8900BootromState *s, uint32_t entry)
{
    switch (entry) {
    case S5L8900_ROM_8900_CHECK:
        s->result = rom_8900_check(s);
        break;
    case S5L8900_ROM_8900_DECRYPT:
        s->result = rom_8900_decrypt(s);
        break;
    case S5L8900_ROM_USB_INIT:
    case S5L8900_ROM_USB_IRQ_27:
    case S5L8900_ROM_USB_IRQ_28:
    case S5L8900_ROM_USB_IRQ_OTG:
    case S5L8900_ROM_USB_START:
        s->result = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8900.bootrom: entry 0x%02x (0x%x, 0x%x, 0x%x)\n", entry, s->args[0],
                      s->args[1], s->args[2]);
        s->result = 0;
        break;
    }
}

static uint64_t rom_io_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900BootromState *s = S5L8900_BOOTROM(opaque);

    switch (addr) {
    case R_ARG0 ... R_ARG2:
        return s->args[addr / 4];
    case R_RESULT:
        return s->result;
    default:
        return 0;
    }
}

static void rom_io_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    S5L8900BootromState *s = S5L8900_BOOTROM(opaque);

    switch (addr) {
    case R_ARG0 ... R_ARG2:
        s->args[addr / 4] = value;
        break;
    case R_CALL:
        rom_call(s, value);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rom_io_ops = {
    .read = rom_io_read,
    .write = rom_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8900_bootrom_realize(DeviceState *dev, Error **errp)
{
    S5L8900BootromState *s = S5L8900_BOOTROM(dev);
    uint32_t *code;

    if (!memory_region_init_rom(&s->code, OBJECT(dev), "s5l8900.bootrom", S5L8900_BOOTROM_SIZE, errp)) {
        return;
    }
    memory_region_init_io(&s->io, OBJECT(dev), &rom_io_ops, s, "s5l8900.bootrom.io", S5L8900_BOOTROM_IO_SIZE);
    memory_region_add_subregion_overlap(&s->code, S5L8900_BOOTROM_IO, &s->io, 1);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->code);

    /*
     * entry n:  b stub_n
     * stub_n:   mov r3, #(n * 4); b call
     * call:     ldr ip, =io; stmia ip, {r0-r3} (r3 last, which makes the call); ldr r0, [ip, #R_RESULT]; bx lr
     */
    code = memory_region_get_ram_ptr(&s->code);
    for (uint32_t n = 0; n < S5L8900_BOOTROM_ENTRIES; n++) {
        uint32_t stub = STUBS + n * STUB_SIZE;
        stl_le_p(&code[n], ARM_B(n * 4, stub));
        stl_le_p(&code[stub / 4], ARM_MOV_R3(n * 4));
        stl_le_p(&code[stub / 4 + 1], ARM_B(stub + 4, CALL));
    }
    stl_le_p(&code[CALL / 4], ARM_LDR_IP_PC(8));
    stl_le_p(&code[CALL / 4 + 1], ARM_STMIA_IP_R0_R3);
    stl_le_p(&code[CALL / 4 + 2], ARM_LDR_R0_IP(R_RESULT));
    stl_le_p(&code[CALL / 4 + 3], ARM_BX_LR);
    // the literal after it, where the registers are, is only known once the machine has mapped the device
}

static void s5l8900_bootrom_reset(DeviceState *dev)
{
    S5L8900BootromState *s = S5L8900_BOOTROM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    uint32_t *code = memory_region_get_ram_ptr(&s->code);

    // the literal of the call: where the registers are
    stl_le_p(&code[CALL / 4 + 4], sbd->mmio[0].addr + S5L8900_BOOTROM_IO);
    memset(s->args, 0, sizeof(s->args));
    s->result = 0;
}

static const VMStateDescription vmstate_s5l8900_bootrom = {
    .name = TYPE_S5L8900_BOOTROM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(args, S5L8900BootromState, 3),
        VMSTATE_UINT32(result, S5L8900BootromState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_bootrom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8900_bootrom_realize;
    dc->vmsd = &vmstate_s5l8900_bootrom;
    device_class_set_legacy_reset(dc, s5l8900_bootrom_reset);
}

static const TypeInfo s5l8900_bootrom_info = {
    .name          = TYPE_S5L8900_BOOTROM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900BootromState),
    .class_init    = s5l8900_bootrom_class_init,
};

static void s5l8900_bootrom_register_types(void)
{
    type_register_static(&s5l8900_bootrom_info);
}

type_init(s5l8900_bootrom_register_types)
