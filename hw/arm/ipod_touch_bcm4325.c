/*
 * Broadcom BCM4325 Wi-Fi card on the S5L8720 SDIO bus, emulated at the protocol level.
 *
 * The AppleBCM4325 kext talks to a "fullmac" dongle: it uploads ARM firmware into the chip's SOCRAM through the
 * function 1 backplane window, releases the ARM core, and then exchanges SDPCM frames over function 2. The firmware is
 * not executed here. The download is accepted and kept (the driver reads the vars back to verify them), and from then
 * on this model acts as the running firmware:
 *  - control channel: BCDC ioctls and iovars, answered with the command, length and transaction id echoed;
 *  - data channel: BDC-framed Ethernet, bridged to a QEMU NIC backend;
 *  - events: Broadcom event frames on the data channel (scan complete, SET_SSID, link up/down).
 * It presents one open access point, BCM4325_AP_SSID on channel BCM4325_AP_CHANNEL.
 *
 * The card interrupt is level triggered: it is asserted while the SDIO core has an interrupt status bit set that the
 * host enabled in its host interrupt mask, and the CCCR enables the interrupt.
 */
#include "hw/arm/ipod_touch_bcm4325.h"
#include "hw/qdev-core.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "trace.h"

static const uint8_t bcm4325_ap_bssid[6] = { 0x02, 0x51, 0x45, 0x4D, 0x55, 0x01 };
static const uint8_t bcm4325_ap_rates[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
static const uint8_t bcm4325_ap_ext_rates[] = { 0x30, 0x48, 0x60, 0x6C };
static const uint8_t brcm_oui[3] = { 0x00, 0x10, 0x18 };
static const char bcm4325_version[] = "wl0: Oct 10 2026 12:00:00 version 4.218.248.0 (QEMU)\n";

static void bcm4325_update_irq(BCM4325State *s)
{
    uint32_t *sdiod = s->core_regs[(BCM4325_SDIOD_BASE - BCM4325_CHIPCOMMON_BASE) / BCM4325_CORE_SIZE];
    bool level = (s->int_enable & 1) && (s->int_enable & 0x6) &&
                 (sdiod[SDIOD_INTSTATUS / 4] & sdiod[SDIOD_HOSTINTMASK / 4]);

    if (level != s->irq_level) {
        s->irq_level = level;
        s->irq_handler(s->irq_opaque, level);
    }
}

static uint32_t *bcm4325_sdiod_reg(BCM4325State *s, uint32_t reg)
{
    return &s->core_regs[(BCM4325_SDIOD_BASE - BCM4325_CHIPCOMMON_BASE) / BCM4325_CORE_SIZE][reg / 4];
}

static void bcm4325_set_intstatus(BCM4325State *s, uint32_t bits)
{
    *bcm4325_sdiod_reg(s, SDIOD_INTSTATUS) |= bits;
    bcm4325_update_irq(s);
}

/*
 * Frames to the host
 */

// Queues an SDPCM frame on a channel. The sequence number and the credit are filled in when the host reads it.
static void bcm4325_queue_frame(BCM4325State *s, uint8_t chan, const uint8_t *data, uint32_t len)
{
    uint16_t frame_len = SDPCM_HDR_LEN + len;
    uint8_t hdr[SDPCM_HDR_LEN] = { 0 };
    GByteArray *frame;

    if (g_queue_get_length(s->rx_frames) >= BCM4325_MAX_RX_FRAMES) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: host is not reading frames, dropping one on channel %u\n", __func__, chan);
        return;
    }

    stw_le_p(hdr, frame_len);
    stw_le_p(hdr + 2, ~frame_len);
    hdr[5] = chan;
    hdr[7] = SDPCM_HDR_LEN;
    frame = g_byte_array_sized_new(frame_len);
    g_byte_array_append(frame, hdr, sizeof(hdr));
    g_byte_array_append(frame, data, len);
    g_queue_push_tail(s->rx_frames, frame);
    trace_ipod_touch_bcm4325_rx_frame(chan, frame_len);

    bcm4325_set_intstatus(s, I_HMB_FRAME_IND);
}

// Serves function 2 reads as a byte stream of the queued frames. An idle card reads as zeros (frame length 0).
static void bcm4325_f2_read(BCM4325State *s, uint8_t *buf, uint32_t len)
{
    uint32_t n;

    memset(buf, 0, len);
    if (!s->rx_cur) {
        s->rx_cur = g_queue_pop_head(s->rx_frames);
        s->rx_pos = 0;
        if (!s->rx_cur) {
            return;
        }
        s->credit = s->tx_seq_next + SDPCM_TX_WINDOW;
        s->rx_cur->data[4] = s->rx_seq++;
        s->rx_cur->data[9] = s->credit;
    }

    n = MIN(len, s->rx_cur->len - s->rx_pos);
    memcpy(buf, s->rx_cur->data + s->rx_pos, n);
    s->rx_pos += len;
    if (s->rx_pos >= s->rx_cur->len) {
        g_byte_array_free(s->rx_cur, true);
        s->rx_cur = NULL;
    }
}

static void bcm4325_queue_ethernet(BCM4325State *s, const uint8_t *eth, uint32_t len)
{
    g_autofree uint8_t *data = g_malloc0(BDC_HDR_LEN + BDC_RX_PAD + len);

    data[0] = BDC_FLAG_VER2;
    memcpy(data + BDC_HDR_LEN + BDC_RX_PAD, eth, len);
    bcm4325_queue_frame(s, SDPCM_CHAN_DATA, data, BDC_HDR_LEN + BDC_RX_PAD + len);
}

static void bcm4325_send_event(BCM4325State *s, uint32_t type, uint32_t status, uint16_t flags, const uint8_t *addr)
{
    uint8_t pkt[14 + 10 + WL_EVENT_MSG_LEN] = { 0 };
    uint8_t *bcm = pkt + 14;
    uint8_t *msg = bcm + 10;

    // Ethernet header
    memcpy(pkt, s->conf.macaddr.a, 6);
    memcpy(pkt + 6, bcm4325_ap_bssid, 6);
    stw_be_p(pkt + 12, ETHER_TYPE_BRCM);
    // bcmeth_hdr_t
    stw_be_p(bcm, BCMILCP_SUBTYPE_VENDOR_LONG);
    stw_be_p(bcm + 2, 6 + WL_EVENT_MSG_LEN);
    bcm[4] = 1;
    memcpy(bcm + 5, brcm_oui, 3);
    stw_be_p(bcm + 8, BCMILCP_BCM_SUBTYPE_EVENT);
    // wl_event_msg_t, big endian
    stw_be_p(msg, 1);
    stw_be_p(msg + 2, flags);
    stl_be_p(msg + 4, type);
    stl_be_p(msg + 8, status);
    memcpy(msg + 24, addr, 6);
    memcpy(msg + 30, "eth0", 4);

    trace_ipod_touch_bcm4325_event(type, status, flags);
    bcm4325_queue_ethernet(s, pkt, sizeof(pkt));
}

/*
 * The virtual access point
 */

// Fills a wl_bss_info_t (version 108) followed by its information elements; returns its length.
static uint32_t bcm4325_build_bss_info(uint8_t *buf, uint32_t size)
{
    uint8_t bss[WL_BSS_INFO_FIXED_LEN + 64] = { 0 };
    uint8_t *ie = bss + WL_BSS_INFO_FIXED_LEN;
    uint32_t ssid_len = strlen(BCM4325_AP_SSID);
    uint32_t len;

    stl_le_p(bss, WL_BSS_INFO_VERSION);
    memcpy(bss + 8, bcm4325_ap_bssid, 6);
    stw_le_p(bss + 14, 100);    // beacon period
    stw_le_p(bss + 16, 0x0401); // capability: ESS, short slot time
    bss[18] = ssid_len;
    memcpy(bss + 19, BCM4325_AP_SSID, ssid_len);
    stl_le_p(bss + 52, sizeof(bcm4325_ap_rates));
    memcpy(bss + 56, bcm4325_ap_rates, sizeof(bcm4325_ap_rates));
    stw_le_p(bss + 72, 0x2B00 | BCM4325_AP_CHANNEL); // chanspec: 2.4 GHz, 20 MHz
    bss[76] = 1;                                     // DTIM period
    stw_le_p(bss + 78, BCM4325_AP_RSSI);
    bss[80] = BCM4325_AP_NOISE;
    bss[88] = BCM4325_AP_CHANNEL;
    stw_le_p(bss + 116, WL_BSS_INFO_FIXED_LEN);

    *ie++ = 0;
    *ie++ = ssid_len;
    memcpy(ie, BCM4325_AP_SSID, ssid_len);
    ie += ssid_len;
    *ie++ = 1;
    *ie++ = sizeof(bcm4325_ap_rates);
    memcpy(ie, bcm4325_ap_rates, sizeof(bcm4325_ap_rates));
    ie += sizeof(bcm4325_ap_rates);
    *ie++ = 3;
    *ie++ = 1;
    *ie++ = BCM4325_AP_CHANNEL;
    *ie++ = 50;
    *ie++ = sizeof(bcm4325_ap_ext_rates);
    memcpy(ie, bcm4325_ap_ext_rates, sizeof(bcm4325_ap_ext_rates));
    ie += sizeof(bcm4325_ap_ext_rates);

    stl_le_p(bss + 120, ie - (bss + WL_BSS_INFO_FIXED_LEN));
    len = ROUND_UP(ie - bss, 4);
    stl_le_p(bss + 4, len);

    memcpy(buf, bss, MIN(len, size));
    return len;
}

// Fills a wl_scan_results_t with the access point; returns its length.
static uint32_t bcm4325_build_scan_results(uint8_t *buf, uint32_t size)
{
    uint8_t results[12 + WL_BSS_INFO_FIXED_LEN + 64];
    uint32_t len = 12 + bcm4325_build_bss_info(results + 12, sizeof(results) - 12);

    stl_le_p(results, len);
    stl_le_p(results + 4, WL_BSS_INFO_VERSION);
    stl_le_p(results + 8, 1);
    memcpy(buf, results, MIN(len, size));
    return len;
}

static void bcm4325_scan_done(void *opaque)
{
    BCM4325State *s = opaque;

    bcm4325_send_event(s, WLC_E_SCAN_COMPLETE, WLC_E_STATUS_SUCCESS, 0, bcm4325_ap_bssid);
}

static void bcm4325_start_scan(BCM4325State *s)
{
    timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * SCALE_MS);
}

static void bcm4325_link_down(BCM4325State *s)
{
    if (s->associated) {
        s->associated = false;
        bcm4325_send_event(s, WLC_E_LINK, WLC_E_STATUS_SUCCESS, 0, bcm4325_ap_bssid);
    }
}

static void bcm4325_join_done(void *opaque)
{
    BCM4325State *s = opaque;

    if (!s->join_ok) {
        bcm4325_send_event(s, WLC_E_SET_SSID, WLC_E_STATUS_NO_NETWORKS, 0, bcm4325_ap_bssid);
        return;
    }

    s->associated = true;
    bcm4325_send_event(s, WLC_E_SET_SSID, WLC_E_STATUS_SUCCESS, 0, bcm4325_ap_bssid);
    bcm4325_send_event(s, WLC_E_LINK, WLC_E_STATUS_SUCCESS, WLC_EVENT_MSG_LINK, bcm4325_ap_bssid);
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

// WLC_SET_SSID with a wlc_ssid_t: joins the access point if the SSID is ours.
static void bcm4325_join(BCM4325State *s, const uint8_t *in, uint32_t inlen)
{
    uint32_t ssid_len = inlen >= 4 ? MIN(ldl_le_p(in), MIN(32, inlen - 4)) : 0;

    if (ssid_len == 0) {
        bcm4325_link_down(s);
        return;
    }

    s->join_ok = ssid_len == strlen(BCM4325_AP_SSID) && !memcmp(in + 4, BCM4325_AP_SSID, ssid_len);
    if (s->associated && !s->join_ok) {
        bcm4325_link_down(s);
    }
    timer_mod(s->join_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20 * SCALE_MS);
}

/*
 * Control channel
 */

static void bcm4325_store(GHashTable *table, gpointer key, const uint8_t *data, uint32_t len)
{
    g_hash_table_replace(table, key, g_bytes_new(data, len));
}

static void bcm4325_load(GHashTable *table, gconstpointer key, uint8_t *out, uint32_t outlen)
{
    GBytes *value = g_hash_table_lookup(table, key);
    gsize len;
    const void *data;

    if (value) {
        data = g_bytes_get_data(value, &len);
        memcpy(out, data, MIN(len, outlen));
    }
}

static void bcm4325_iovar(BCM4325State *s, bool set, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outlen)
{
    uint32_t name_len = strnlen((const char *)in, inlen);
    g_autofree char *name = g_strndup((const char *)in, name_len);
    const uint8_t *value = in + MIN(name_len + 1, inlen);
    uint32_t value_len = inlen - (value - in);

    trace_ipod_touch_bcm4325_iovar(name, set, set ? value_len : outlen);

    if (set) {
        if (!strcmp(name, "iscan") && value_len >= 6) {
            // wl_iscan_params_t: version, action (1 start, 2 continue, 3 abort), scan duration, scan params
            uint16_t action = lduw_le_p(value + 4);
            if (action == 1 || action == 2) {
                bcm4325_start_scan(s);
            }
            return;
        }
        bcm4325_store(s->iovars, g_strdup(name), value, value_len);
        return;
    }

    memset(out, 0, outlen);
    if (!strcmp(name, "ver")) {
        pstrcpy((char *)out, outlen, bcm4325_version);
    } else if (!strcmp(name, "cur_etheraddr")) {
        memcpy(out, s->conf.macaddr.a, MIN(6, outlen));
    } else if (!strcmp(name, "iscanresults")) {
        // wl_iscan_results_t: status, then wl_scan_results_t
        if (outlen >= 4) {
            stl_le_p(out, WL_SCAN_RESULTS_SUCCESS);
            bcm4325_build_scan_results(out + 4, outlen - 4);
        }
    } else {
        bcm4325_load(s->iovars, name, out, outlen);
    }
}

// Answers an ioctl; out starts as a copy of the input. Returns false to flag an error to the driver.
static bool bcm4325_ioctl(BCM4325State *s, uint32_t cmd, bool set, const uint8_t *in, uint32_t inlen,
                          uint8_t *out, uint32_t outlen)
{
    if (cmd == WLC_GET_VAR || cmd == WLC_SET_VAR) {
        bcm4325_iovar(s, cmd == WLC_SET_VAR, in, inlen, out, outlen);
        return true;
    }

    trace_ipod_touch_bcm4325_ioctl(cmd, set, set ? inlen : outlen);

    switch (cmd) {
    case WLC_UP:
    case WLC_DOWN:
        return true;
    case WLC_SET_SSID:
        bcm4325_join(s, in, inlen);
        return true;
    case WLC_SCAN:
        bcm4325_start_scan(s);
        return true;
    case WLC_DISASSOC:
        bcm4325_link_down(s);
        return true;
    }

    if (set) {
        // the GET counterpart of a SET ioctl has the number before it
        bcm4325_store(s->ioctls, GUINT_TO_POINTER(cmd - 1), in, inlen);
        return true;
    }

    memset(out, 0, outlen);
    switch (cmd) {
    case WLC_GET_MAGIC:
        if (outlen >= 4) {
            stl_le_p(out, WLC_IOCTL_MAGIC);
        }
        break;
    case WLC_GET_VERSION:
        if (outlen >= 4) {
            stl_le_p(out, WLC_IOCTL_VERSION);
        }
        break;
    case WLC_GET_RATE:
        if (outlen >= 4) {
            stl_le_p(out, s->associated ? 108 : 0); // 54 Mbps in units of 500 kbps
        }
        break;
    case WLC_GET_BSSID:
        if (s->associated) {
            memcpy(out, bcm4325_ap_bssid, MIN(6, outlen));
        }
        break;
    case WLC_GET_SSID:
        if (s->associated && outlen >= 4 + strlen(BCM4325_AP_SSID)) {
            stl_le_p(out, strlen(BCM4325_AP_SSID));
            memcpy(out + 4, BCM4325_AP_SSID, strlen(BCM4325_AP_SSID));
        }
        break;
    case WLC_GET_CHANNEL:
        // channel_info_t: hardware channel, target channel, scan channel
        if (outlen >= 8) {
            stl_le_p(out, BCM4325_AP_CHANNEL);
            stl_le_p(out + 4, BCM4325_AP_CHANNEL);
        }
        break;
    case WLC_GET_PHYTYPE:
        if (outlen >= 4) {
            stl_le_p(out, WLC_PHY_TYPE_G);
        }
        break;
    case WLC_SCAN_RESULTS:
        bcm4325_build_scan_results(out, outlen);
        break;
    case WLC_GET_RSSI:
        if (s->associated && outlen >= 4) {
            stl_le_p(out, BCM4325_AP_RSSI);
        }
        break;
    case WLC_GET_BSS_INFO:
        // the length of the buffer, then the wl_bss_info_t
        if (outlen >= 4) {
            stl_le_p(out, outlen);
            bcm4325_build_bss_info(out + 4, outlen - 4);
        }
        break;
    case WLC_GET_BANDLIST:
        if (outlen >= 8) {
            stl_le_p(out, 1);
            stl_le_p(out + 4, 2); // 2.4 GHz
        }
        break;
    case WLC_GET_VALID_CHANNELS:
        if (outlen >= 4 * 12) {
            stl_le_p(out, 11);
            for (int i = 1; i <= 11; i++) {
                stl_le_p(out + 4 * i, i);
            }
        }
        break;
    default:
        bcm4325_load(s->ioctls, GUINT_TO_POINTER(cmd), out, outlen);
        break;
    }
    return true;
}

// A BCDC message on the control channel: answers it with the command, length and transaction id echoed.
static void bcm4325_handle_control(BCM4325State *s, const uint8_t *msg, uint32_t len)
{
    uint32_t cmd, outlen, flags;
    g_autofree uint8_t *resp = NULL;
    bool ok;

    if (len < BCDC_HDR_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: short control message (%u bytes)\n", __func__, len);
        return;
    }
    cmd = ldl_le_p(msg);
    outlen = ldl_le_p(msg + 4);
    flags = ldl_le_p(msg + 8);
    if (outlen > 0x8000) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: ioctl %u wants %u bytes back\n", __func__, cmd, outlen);
        return;
    }

    resp = g_malloc0(BCDC_HDR_LEN + outlen);
    memcpy(resp + BCDC_HDR_LEN, msg + BCDC_HDR_LEN, MIN(len - BCDC_HDR_LEN, outlen));
    ok = bcm4325_ioctl(s, cmd, flags & BCDC_FLAG_SET, msg + BCDC_HDR_LEN, len - BCDC_HDR_LEN,
                       resp + BCDC_HDR_LEN, outlen);

    stl_le_p(resp, cmd);
    stl_le_p(resp + 4, outlen);
    stl_le_p(resp + 8, (flags & ~BCDC_FLAG_ERROR) | (ok ? 0 : BCDC_FLAG_ERROR));
    bcm4325_queue_frame(s, SDPCM_CHAN_CONTROL, resp, BCDC_HDR_LEN + outlen);
}

// A BDC-framed Ethernet frame on the data channel: sends it to the network.
static void bcm4325_handle_data(BCM4325State *s, const uint8_t *data, uint32_t len)
{
    uint32_t offset;

    if (len < BDC_HDR_LEN) {
        return;
    }
    offset = BDC_HDR_LEN + data[3] * 4;
    if (offset >= len || !s->associated) {
        return;
    }
    qemu_send_packet(qemu_get_queue(s->nic), data + offset, len - offset);
}

// A frame from the host on function 2.
static void bcm4325_f2_write(BCM4325State *s, const uint8_t *buf, uint32_t len)
{
    uint16_t frame_len, offset;
    uint8_t chan;

    if (len < SDPCM_HDR_LEN) {
        return;
    }
    frame_len = lduw_le_p(buf);
    if ((frame_len ^ lduw_le_p(buf + 2)) != 0xFFFF || frame_len < SDPCM_HDR_LEN || frame_len > len) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad SDPCM header (length 0x%04x, check 0x%04x, %u bytes)\n", __func__,
                      frame_len, lduw_le_p(buf + 2), len);
        return;
    }
    chan = buf[5] & 0xF;
    offset = MAX(buf[7], SDPCM_HDR_LEN);
    s->tx_seq_next = buf[4] + 1;
    trace_ipod_touch_bcm4325_tx_frame(chan, buf[4], frame_len);

    if (!s->fw_running) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: frame before the firmware runs\n", __func__);
        return;
    }
    if (offset > frame_len) {
        return;
    }

    switch (chan) {
    case SDPCM_CHAN_CONTROL:
        bcm4325_handle_control(s, buf + offset, frame_len - offset);
        break;
    case SDPCM_CHAN_DATA:
        bcm4325_handle_data(s, buf + offset, frame_len - offset);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: frame on channel %u\n", __func__, chan);
        break;
    }

    // Keep the host's transmit window open: send a header-only frame with new credit if nothing else will carry it.
    if ((uint8_t)(s->credit - s->tx_seq_next) < SDPCM_TX_WINDOW / 2 && !s->rx_cur &&
        g_queue_is_empty(s->rx_frames)) {
        bcm4325_queue_frame(s, SDPCM_CHAN_EVENT, NULL, 0);
    }
}

/*
 * Backplane
 */

static void bcm4325_reset_backplane(BCM4325State *s)
{
    memset(s->core_regs, 0, sizeof(s->core_regs));
    for (int i = 0; i < BCM4325_NUM_CORES; i++) {
        s->core_regs[i][SB_TMSTATELOW / 4] = SB_TMSTATELOW_RESET;
    }
    s->core_regs[(BCM4325_SOCRAM_BASE - BCM4325_CHIPCOMMON_BASE) / BCM4325_CORE_SIZE][0] = 0x25033; // core info
    s->core_regs[(BCM4325_SOCRAM_BASE - BCM4325_CHIPCOMMON_BASE) / BCM4325_CORE_SIZE][SB_IDHIGH / 4] = 0x6000225D;
    s->fw_running = false;
    s->associated = false;
    timer_del(s->scan_timer);
    timer_del(s->join_timer);

    while (!g_queue_is_empty(s->rx_frames)) {
        g_byte_array_free(g_queue_pop_head(s->rx_frames), true);
    }
    if (s->rx_cur) {
        g_byte_array_free(s->rx_cur, true);
        s->rx_cur = NULL;
    }
    s->rx_seq = 0;
    s->tx_seq_next = 0;
    s->credit = 0;
    g_hash_table_remove_all(s->ioctls);
    g_hash_table_remove_all(s->iovars);
}

// The firmware starts running: it reports the SDPCM protocol version through the to-host mailbox.
static void bcm4325_firmware_start(BCM4325State *s)
{
    s->fw_running = true;
    trace_ipod_touch_bcm4325_firmware_start();
    *bcm4325_sdiod_reg(s, SDIOD_TOHOSTMAILBOXDATA) = SDPCM_PROT_VERSION_DATA;
    bcm4325_set_intstatus(s, I_HMB_HOST_INT);
}

static int bcm4325_core_index(uint32_t addr)
{
    if (addr < BCM4325_CHIPCOMMON_BASE || addr >= BCM4325_CHIPCOMMON_BASE + BCM4325_NUM_CORES * BCM4325_CORE_SIZE) {
        return -1;
    }
    return (addr - BCM4325_CHIPCOMMON_BASE) / BCM4325_CORE_SIZE;
}

static uint32_t bcm4325_bp_read32(BCM4325State *s, uint32_t addr)
{
    int core = bcm4325_core_index(addr);
    uint32_t reg = addr & (BCM4325_CORE_SIZE - 4);

    if (addr <= BCM4325_SOCRAM_SIZE - 4) {
        return ldl_le_p(s->socram + addr);
    }
    if (core < 0) {
        qemu_log_mask(LOG_UNIMP, "%s: unknown backplane address 0x%08x\n", __func__, addr);
        return 0;
    }
    if (addr == BCM4325_CHIPCOMMON_BASE + CC_CHIPID) {
        return BCM4325_CHIPID;
    }
    return s->core_regs[core][reg / 4];
}

static void bcm4325_bp_write32(BCM4325State *s, uint32_t addr, uint32_t val)
{
    int core = bcm4325_core_index(addr);
    uint32_t reg = addr & (BCM4325_CORE_SIZE - 4);
    uint32_t old;

    if (addr <= BCM4325_SOCRAM_SIZE - 4) {
        stl_le_p(s->socram + addr, val);
        return;
    }
    if (core < 0) {
        qemu_log_mask(LOG_UNIMP, "%s: unknown backplane address 0x%08x (value 0x%08x)\n", __func__, addr, val);
        return;
    }

    old = s->core_regs[core][reg / 4];
    s->core_regs[core][reg / 4] = val;
    switch (addr) {
    case BCM4325_CHIPCOMMON_BASE + CC_WATCHDOG:
        if (val) {
            trace_ipod_touch_bcm4325_watchdog_reset();
            bcm4325_reset_backplane(s);
            bcm4325_update_irq(s);
        }
        break;
    case BCM4325_SDIOD_BASE + SDIOD_INTSTATUS:
        s->core_regs[core][reg / 4] = old & ~val;
        bcm4325_update_irq(s);
        break;
    case BCM4325_SDIOD_BASE + SDIOD_HOSTINTMASK:
        bcm4325_update_irq(s);
        break;
    case BCM4325_ARM_BASE + SB_TMSTATELOW:
        // the ARM core leaves reset with its clock enabled: the downloaded firmware starts
        if (!s->fw_running && (old & SB_TMSTATELOW_RESET) &&
            (val & (SB_TMSTATELOW_RESET | SB_TMSTATELOW_CLOCK)) == SB_TMSTATELOW_CLOCK) {
            bcm4325_firmware_start(s);
        }
        break;
    }
}

static uint8_t bcm4325_bp_read8(BCM4325State *s, uint32_t addr)
{
    if (addr < BCM4325_SOCRAM_SIZE) {
        return s->socram[addr];
    }
    return bcm4325_bp_read32(s, addr & ~3) >> ((addr & 3) * 8);
}

static void bcm4325_bp_write8(BCM4325State *s, uint32_t addr, uint8_t val)
{
    uint32_t shift = (addr & 3) * 8;
    uint32_t word;

    if (addr < BCM4325_SOCRAM_SIZE) {
        s->socram[addr] = val;
        return;
    }
    if ((addr & ~3) == BCM4325_SDIOD_BASE + SDIOD_INTSTATUS) {
        // write one to clear: leave the other bytes alone
        bcm4325_bp_write32(s, addr & ~3, (uint32_t)val << shift);
        return;
    }
    word = bcm4325_bp_read32(s, addr & ~3);
    bcm4325_bp_write32(s, addr & ~3, (word & ~(0xFFu << shift)) | ((uint32_t)val << shift));
}

/*
 * SDIO functions
 */

static uint8_t bcm4325_f1_reg_read(BCM4325State *s, uint32_t addr)
{
    switch (addr) {
    case SBSDIO_FUNC1_SBADDRLOW:
        return (s->sb_window >> 8) & 0x80;
    case SBSDIO_FUNC1_SBADDRMID:
        return s->sb_window >> 16;
    case SBSDIO_FUNC1_SBADDRHIGH:
        return s->sb_window >> 24;
    case SBSDIO_FUNC1_CHIPCLKCSR:
        // the ALP and HT clocks are always available
        return (s->f1_regs[addr - SBSDIO_F1_REGS_BASE] & 0x3F) | SBSDIO_ALP_AVAIL | SBSDIO_HT_AVAIL;
    default:
        if (addr - SBSDIO_F1_REGS_BASE < SBSDIO_F1_REGS_SIZE) {
            return s->f1_regs[addr - SBSDIO_F1_REGS_BASE];
        }
        qemu_log_mask(LOG_UNIMP, "%s: unknown function 1 register 0x%05x\n", __func__, addr);
        return 0;
    }
}

static void bcm4325_f1_reg_write(BCM4325State *s, uint32_t addr, uint8_t val)
{
    switch (addr) {
    case SBSDIO_FUNC1_SBADDRLOW:
        s->sb_window = (s->sb_window & ~0x8000) | ((val & 0x80) << 8);
        break;
    case SBSDIO_FUNC1_SBADDRMID:
        s->sb_window = (s->sb_window & ~0xFF0000) | (val << 16);
        break;
    case SBSDIO_FUNC1_SBADDRHIGH:
        s->sb_window = (s->sb_window & ~0xFF000000) | ((uint32_t)val << 24);
        break;
    default:
        if (addr - SBSDIO_F1_REGS_BASE < SBSDIO_F1_REGS_SIZE) {
            s->f1_regs[addr - SBSDIO_F1_REGS_BASE] = val;
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: unknown function 1 register 0x%05x (value 0x%02x)\n", __func__, addr, val);
        }
        break;
    }
}

static uint32_t bcm4325_bp_addr(BCM4325State *s, uint32_t addr)
{
    return s->sb_window | (addr & SBSDIO_SB_OFT_ADDR_MASK);
}

static uint8_t bcm4325_f0_read(BCM4325State *s, uint32_t addr)
{
    uint32_t func = addr / FBR_SIZE;

    if (addr >= CIS_BASE && addr < CIS_BASE + sizeof(s->cis)) {
        return s->cis[addr - CIS_BASE];
    }
    if (func >= 1 && func <= BCM4325_NUM_FUNCTIONS) {
        switch (addr % FBR_SIZE) {
        case FBR_CIS_PTR:
        case FBR_CIS_PTR + 1:
        case FBR_CIS_PTR + 2:
            return (CIS_BASE + func * CIS_SIZE) >> ((addr % FBR_SIZE - FBR_CIS_PTR) * 8);
        case FBR_BLKSIZE:
            return s->blksize[func];
        case FBR_BLKSIZE + 1:
            return s->blksize[func] >> 8;
        default:
            return 0;
        }
    }

    switch (addr) {
    case CCCR_REVISION:
        return 0x32; // CCCR 1.20, SDIO 2.00
    case CCCR_SD_REVISION:
        return 0x02;
    case CCCR_IO_ENABLE:
        return s->io_enable;
    case CCCR_IO_READY:
        return s->io_enable;
    case CCCR_INT_ENABLE:
        return s->int_enable;
    case CCCR_INT_PENDING:
        return s->irq_level ? 1 << 2 : 0;
    case CCCR_BUS_IF:
        return s->bus_if;
    case CCCR_CAPABILITY:
        return 0x12; // multi-block transfers, 4-bit interrupt between blocks
    case CCCR_CIS_PTR:
    case CCCR_CIS_PTR + 1:
    case CCCR_CIS_PTR + 2:
        return CIS_BASE >> ((addr - CCCR_CIS_PTR) * 8);
    case FBR_BLKSIZE:
        return s->blksize[0];
    case FBR_BLKSIZE + 1:
        return s->blksize[0] >> 8;
    case CCCR_HIGH_SPEED:
        return s->high_speed;
    default:
        return 0;
    }
}

static void bcm4325_f0_write(BCM4325State *s, uint32_t addr, uint8_t val)
{
    uint32_t func = addr / FBR_SIZE;

    if (func <= BCM4325_NUM_FUNCTIONS && (addr % FBR_SIZE == FBR_BLKSIZE || addr % FBR_SIZE == FBR_BLKSIZE + 1)) {
        uint32_t shift = (addr % FBR_SIZE - FBR_BLKSIZE) * 8;
        s->blksize[func] = (s->blksize[func] & ~(0xFF << shift)) | (val << shift);
        return;
    }

    switch (addr) {
    case CCCR_IO_ENABLE:
        s->io_enable = val & 0x6;
        break;
    case CCCR_INT_ENABLE:
        s->int_enable = val & 0x7;
        bcm4325_update_irq(s);
        break;
    case CCCR_IO_ABORT:
        break;
    case CCCR_BUS_IF:
        s->bus_if = val;
        break;
    case CCCR_HIGH_SPEED:
        s->high_speed = (val & 0x2) | 0x1;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write of 0x%02x to function 0 register 0x%05x\n", __func__, val, addr);
        break;
    }
}

static uint8_t bcm4325_io_rw_direct(BCM4325State *s, uint32_t arg)
{
    uint32_t func = (arg >> SDIO_ARG_FUNC_SHIFT) & SDIO_ARG_FUNC_MASK;
    uint32_t addr = (arg >> SDIO_ARG_ADDR_SHIFT) & SDIO_ARG_ADDR_MASK;
    uint8_t val = arg & 0xFF;

    if (arg & SDIO_ARG_WRITE) {
        switch (func) {
        case 0:
            bcm4325_f0_write(s, addr, val);
            break;
        case 1:
            if (addr >= SBSDIO_F1_REGS_BASE) {
                bcm4325_f1_reg_write(s, addr, val);
            } else {
                bcm4325_bp_write8(s, bcm4325_bp_addr(s, addr), val);
            }
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: CMD52 write to function %u\n", __func__, func);
            break;
        }
    }

    switch (func) {
    case 0:
        return bcm4325_f0_read(s, addr);
    case 1:
        if (addr >= SBSDIO_F1_REGS_BASE) {
            return bcm4325_f1_reg_read(s, addr);
        }
        return bcm4325_bp_read8(s, bcm4325_bp_addr(s, addr));
    default:
        return 0;
    }
}

void bcm4325_io_rw_extended(BCM4325State *s, uint32_t arg, uint8_t *buf, uint32_t len)
{
    uint32_t func = (arg >> SDIO_ARG_FUNC_SHIFT) & SDIO_ARG_FUNC_MASK;
    uint32_t addr = (arg >> SDIO_ARG_ADDR_SHIFT) & SDIO_ARG_ADDR_MASK;
    bool write = arg & SDIO_ARG_WRITE;
    bool incr = arg & SDIO_ARG_INCR_ADDR;

    trace_ipod_touch_bcm4325_cmd53(write, func, addr, len);

    switch (func) {
    case 1:
        if (addr < SBSDIO_F1_REGS_BASE && len == 4 && !(addr & 3)) {
            // a 32-bit backplane register access
            uint32_t bp_addr = bcm4325_bp_addr(s, addr);
            if (write) {
                bcm4325_bp_write32(s, bp_addr, ldl_le_p(buf));
            } else {
                stl_le_p(buf, bcm4325_bp_read32(s, bp_addr));
            }
            break;
        }
        for (uint32_t i = 0; i < len; i++) {
            uint32_t a = incr ? addr + i : addr;
            if (a >= SBSDIO_F1_REGS_BASE) {
                if (write) {
                    bcm4325_f1_reg_write(s, a, buf[i]);
                } else {
                    buf[i] = bcm4325_f1_reg_read(s, a);
                }
            } else if (write) {
                bcm4325_bp_write8(s, bcm4325_bp_addr(s, a), buf[i]);
            } else {
                buf[i] = bcm4325_bp_read8(s, bcm4325_bp_addr(s, a));
            }
        }
        break;
    case 2:
        if (write) {
            bcm4325_f2_write(s, buf, len);
        } else {
            bcm4325_f2_read(s, buf, len);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: CMD53 on function %u\n", __func__, func);
        if (!write) {
            memset(buf, 0, len);
        }
        break;
    }
}

bool bcm4325_command(BCM4325State *s, uint8_t index, uint32_t arg, uint32_t *resp)
{
    switch (index) {
    case SD_CMD_IO_SEND_OP_COND:
        // R4: ready, two I/O functions, no memory, 2.0-3.6 V
        *resp = (1u << 31) | (BCM4325_NUM_FUNCTIONS << 28) | 0xFF8000;
        return true;
    case SD_CMD_SEND_RELATIVE_ADDR:
        // R6: the new relative card address
        s->rca = 1;
        *resp = s->rca << 16;
        return true;
    case SD_CMD_SELECT_CARD:
        *resp = 0x1E00; // R1b: transfer state, ready for data
        return true;
    case SD_CMD_IO_RW_DIRECT:
        // R5: response flags (I/O state CMD), then the data
        *resp = 0x1000 | bcm4325_io_rw_direct(s, arg);
        return true;
    case SD_CMD_IO_RW_EXTENDED:
        *resp = 0x2000; // R5: I/O state TRN
        return true;
    case 0: // GO_IDLE_STATE
        *resp = 0;
        return true;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown command %u (argument 0x%08x)\n", __func__, index, arg);
        return false;
    }
}

/*
 * Network backend
 */

static bool bcm4325_can_receive(NetClientState *nc)
{
    BCM4325State *s = qemu_get_nic_opaque(nc);
    return !s->associated || g_queue_get_length(s->rx_frames) < BCM4325_MAX_RX_FRAMES / 2;
}

static ssize_t bcm4325_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    BCM4325State *s = qemu_get_nic_opaque(nc);

    // without a link the frame is lost in the air
    if (s->associated && size <= BCM4325_ETH_MTU) {
        bcm4325_queue_ethernet(s, buf, size);
    }
    return size;
}

static NetClientInfo net_bcm4325_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = bcm4325_can_receive,
    .receive = bcm4325_receive,
};

// The function 0 CIS carries the identity and the MAC address (a LAN node id FUNCE tuple, which the driver reads as
// its OTP); the function CISes are empty.
static void bcm4325_build_cis(BCM4325State *s)
{
    uint8_t *p = s->cis;

    memset(s->cis, CISTPL_END, sizeof(s->cis));
    *p++ = CISTPL_MANFID;
    *p++ = 4;
    stw_le_p(p, BCM4325_MANUFACTURER_ID);
    stw_le_p(p + 2, BCM4325_PRODUCT_ID);
    p += 4;
    *p++ = CISTPL_FUNCE;
    *p++ = 8;
    *p++ = CISTPL_FUNCE_LAN_NODE_ID;
    *p++ = 6;
    memcpy(p, s->conf.macaddr.a, 6);
}

void bcm4325_reset(BCM4325State *s)
{
    s->rca = 0;
    s->io_enable = 0;
    s->int_enable = 0;
    s->bus_if = 0;
    s->high_speed = 0x1; // supports high speed
    memset(s->blksize, 0, sizeof(s->blksize));
    s->sb_window = 0;
    memset(s->f1_regs, 0, sizeof(s->f1_regs));
    memset(s->socram, 0, BCM4325_SOCRAM_SIZE);
    bcm4325_reset_backplane(s);
    s->irq_level = false;
    s->irq_handler(s->irq_opaque, false);
}

void bcm4325_init(BCM4325State *s, DeviceState *owner, BCM4325IRQHandler handler, void *opaque)
{
    s->irq_handler = handler;
    s->irq_opaque = opaque;
    s->socram = g_malloc0(BCM4325_SOCRAM_SIZE);
    s->rx_frames = g_queue_new();
    s->ioctls = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_bytes_unref);
    s->iovars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bcm4325_scan_done, s);
    s->join_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bcm4325_join_done, s);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    bcm4325_build_cis(s);

    s->nic = qemu_new_nic(&net_bcm4325_info, &s->conf, "bcm4325", owner->id, &owner->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    bcm4325_reset(s);
}
