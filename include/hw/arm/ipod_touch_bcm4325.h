#ifndef HW_ARM_IPOD_TOUCH_BCM4325_H
#define HW_ARM_IPOD_TOUCH_BCM4325_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "net/net.h"

// SDIO card identity, as the AppleBCM4325 personality matches it (from the CIS MANFID tuple)
#define BCM4325_MANUFACTURER_ID 0x4D50
#define BCM4325_PRODUCT_ID      0x4D48
#define BCM4325_NUM_FUNCTIONS   2

// SDIO commands
#define SD_CMD_SEND_RELATIVE_ADDR 3
#define SD_CMD_IO_SEND_OP_COND    5
#define SD_CMD_SELECT_CARD        7
#define SD_CMD_IO_RW_DIRECT       52
#define SD_CMD_IO_RW_EXTENDED     53

// CMD52 / CMD53 argument fields
#define SDIO_ARG_WRITE       (1u << 31)
#define SDIO_ARG_FUNC_SHIFT  28
#define SDIO_ARG_FUNC_MASK   0x7
#define SDIO_ARG_BLOCK_MODE  (1u << 27)
#define SDIO_ARG_INCR_ADDR   (1u << 26)
#define SDIO_ARG_ADDR_SHIFT  9
#define SDIO_ARG_ADDR_MASK   0x1FFFF
#define SDIO_ARG_COUNT_MASK  0x1FF

// CCCR registers (function 0)
#define CCCR_REVISION    0x00
#define CCCR_SD_REVISION 0x01
#define CCCR_IO_ENABLE   0x02
#define CCCR_IO_READY    0x03
#define CCCR_INT_ENABLE  0x04
#define CCCR_INT_PENDING 0x05
#define CCCR_IO_ABORT    0x06
#define CCCR_BUS_IF      0x07
#define CCCR_CAPABILITY  0x08
#define CCCR_CIS_PTR     0x09
#define CCCR_POWER       0x12
#define CCCR_HIGH_SPEED  0x13
#define FBR_SIZE         0x100
#define FBR_CIS_PTR      0x09
#define FBR_BLKSIZE      0x10
#define CIS_BASE         0x1000 // CIS of function n at CIS_BASE + n * CIS_SIZE
#define CIS_SIZE         0x100

// CIS tuples
#define CISTPL_MANFID 0x20
#define CISTPL_FUNCE  0x22
#define CISTPL_END    0xFF
#define CISTPL_FUNCE_LAN_NODE_ID 0x04

// Function 1 registers above the backplane window
#define SBSDIO_WATERMARK        0x10008
#define SBSDIO_FUNC1_SBADDRLOW  0x1000A
#define SBSDIO_FUNC1_SBADDRMID  0x1000B
#define SBSDIO_FUNC1_SBADDRHIGH 0x1000C
#define SBSDIO_FUNC1_FRAMECTRL  0x1000D
#define SBSDIO_FUNC1_CHIPCLKCSR 0x1000E
#define SBSDIO_FUNC1_SDIOPULLUP 0x1000F
#define SBSDIO_F1_REGS_BASE     0x10000
#define SBSDIO_F1_REGS_SIZE     0x20
#define SBSDIO_SB_OFT_ADDR_MASK 0x7FFF
#define SBSDIO_SBWINDOW_MASK    0xFFFF8000

// chip clock CSR
#define SBSDIO_ALP_AVAIL (1 << 6)
#define SBSDIO_HT_AVAIL  (1 << 7)

// backplane address map
#define BCM4325_SOCRAM_SIZE     0x60000
#define BCM4325_CHIPCOMMON_BASE 0x18000000
#define BCM4325_SDIOD_BASE      0x18002000
#define BCM4325_ARM_BASE        0x18003000
#define BCM4325_SOCRAM_BASE     0x18004000
#define BCM4325_CORE_SIZE       0x1000
#define BCM4325_NUM_CORES       5
#define CC_CHIPID               0x000
#define CC_WATCHDOG             0x634
#define SDIOD_INTSTATUS         0x020
#define SDIOD_HOSTINTMASK       0x024
#define SDIOD_TOSBMAILBOXDATA   0x048
#define SDIOD_TOHOSTMAILBOXDATA 0x04C
#define SB_TMSTATELOW           0xF98 // core wrapper control: bit 0 reset, bit 16 clock enable
#define SB_TMSTATELOW_RESET     (1 << 0)
#define SB_TMSTATELOW_CLOCK     (1 << 16)
#define SB_IDHIGH               0xFF8

// chip id: BCM4325 revision D0, 5 cores
#define BCM4325_CHIPID 0x05054325

// SDIO core interrupt status bits
#define I_HMB_FC_CHANGE (1 << 5)
#define I_HMB_FRAME_IND (1 << 6)
#define I_HMB_HOST_INT  (1 << 7) // to-host mailbox
#define SDPCM_PROT_VERSION_DATA (4 << 16)

// SDPCM framing on function 2
#define SDPCM_HDR_LEN      12
#define SDPCM_CHAN_CONTROL 0
#define SDPCM_CHAN_EVENT   1
#define SDPCM_CHAN_DATA    2
#define SDPCM_CHAN_DEBUG   0xF
#define SDPCM_TX_WINDOW    32 // frames the host may send ahead of the last one we consumed

// BCDC protocol
#define BCDC_HDR_LEN    12
#define BCDC_FLAG_ERROR (1 << 0)
#define BCDC_FLAG_SET   (1 << 1)
#define BCDC_ID_SHIFT   16
#define BDC_HDR_LEN     4
#define BDC_FLAG_VER2   0x20
#define BDC_RX_PAD      2 // the driver expects the Ethernet header 6 bytes into a received data frame

// Broadcom ioctls
#define WLC_GET_MAGIC          0
#define WLC_GET_VERSION        1
#define WLC_UP                 2
#define WLC_DOWN               3
#define WLC_GET_RATE           12
#define WLC_GET_INFRA          19
#define WLC_GET_BSSID          23
#define WLC_GET_SSID           25
#define WLC_SET_SSID           26
#define WLC_GET_CHANNEL        29
#define WLC_GET_PHYTYPE        39
#define WLC_SCAN               50
#define WLC_SCAN_RESULTS       51
#define WLC_DISASSOC           52
#define WLC_GET_COUNTRY        83
#define WLC_GET_RSSI           127
#define WLC_GET_BSS_INFO       136
#define WLC_GET_BANDLIST       140
#define WLC_GET_VALID_CHANNELS 217
#define WLC_GET_VAR            262
#define WLC_SET_VAR            263
#define WLC_IOCTL_MAGIC        0x14E46C77
#define WLC_IOCTL_VERSION      1
#define WLC_PHY_TYPE_G         2

// events
#define WLC_E_SET_SSID      0
#define WLC_E_AUTH          3
#define WLC_E_ASSOC         7
#define WLC_E_DISASSOC_IND  12
#define WLC_E_LINK          16
#define WLC_E_SCAN_COMPLETE 26
#define WLC_E_STATUS_SUCCESS     0
#define WLC_E_STATUS_FAIL        1
#define WLC_E_STATUS_NO_NETWORKS 3
#define WLC_EVENT_MSG_LINK  0x01
#define ETHER_TYPE_BRCM     0x886C
#define BCMILCP_SUBTYPE_VENDOR_LONG 0x8001
#define BCMILCP_BCM_SUBTYPE_EVENT   1
#define WL_EVENT_MSG_LEN    46 // wl_event_msg_t: 7 fields, the address and the interface name

// scan results
#define WL_BSS_INFO_VERSION 108
#define WL_BSS_INFO_FIXED_LEN 0x7C
#define WL_SCAN_RESULTS_SUCCESS 0

// the virtual access point
#define BCM4325_AP_SSID    "QEMU"
#define BCM4325_AP_CHANNEL 6
#define BCM4325_AP_RSSI    (-40)
#define BCM4325_AP_NOISE   (-92)

#define BCM4325_MAX_RX_FRAMES 256
#define BCM4325_ETH_MTU       1514

typedef void (*BCM4325IRQHandler)(void *opaque, bool level);

typedef struct BCM4325State {
    // SDIO function 0
    uint16_t rca;
    uint8_t io_enable;
    uint8_t int_enable;
    uint8_t bus_if;
    uint8_t high_speed;
    uint16_t blksize[BCM4325_NUM_FUNCTIONS + 1];
    uint8_t cis[(BCM4325_NUM_FUNCTIONS + 1) * CIS_SIZE];

    // function 1: backplane window and local registers
    uint32_t sb_window;
    uint8_t f1_regs[SBSDIO_F1_REGS_SIZE];

    // backplane
    uint8_t *socram;
    uint32_t core_regs[BCM4325_NUM_CORES][BCM4325_CORE_SIZE / 4];
    bool fw_running;

    // function 2: SDPCM frames to the host, one being read at a time
    GQueue *rx_frames;
    GByteArray *rx_cur;
    uint32_t rx_pos;
    uint8_t rx_seq;
    uint8_t tx_seq_next; // the sequence number the host will send next
    uint8_t credit;      // the last credit given to the host
    bool irq_level;

    // dongle state
    GHashTable *ioctls; // set values by the GET ioctl number
    GHashTable *iovars; // set values by name
    MACAddr bssid;
    bool associated;
    QEMUTimer *scan_timer;
    QEMUTimer *join_timer;
    bool join_ok;

    NICState *nic;
    NICConf conf;

    BCM4325IRQHandler irq_handler;
    void *irq_opaque;
} BCM4325State;

void bcm4325_init(BCM4325State *s, DeviceState *owner, BCM4325IRQHandler handler, void *opaque);
void bcm4325_reset(BCM4325State *s);
// Executes a command without data; returns false if the card does not respond.
bool bcm4325_command(BCM4325State *s, uint8_t index, uint32_t arg, uint32_t *resp);
// Transfers the data of a CMD53: fills buf for a read, consumes it for a write.
void bcm4325_io_rw_extended(BCM4325State *s, uint32_t arg, uint8_t *buf, uint32_t len);

#endif
