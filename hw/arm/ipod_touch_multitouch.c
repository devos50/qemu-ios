#include "hw/arm/ipod_touch_multitouch.h"
#include "hw/irq.h"
#include "qemu/log.h"

#define MT_IO_BUFFER_SIZE 0x100

static void log_unknown_command(IPodTouchMultitouchState *s, uint8_t cmd, const char *what) {
    if(!(s->unknown_cmd_logged[cmd >> 3] & (1 << (cmd & 7)))) {
        s->unknown_cmd_logged[cmd >> 3] |= 1 << (cmd & 7);
        qemu_log_mask(LOG_UNIMP, "%s: unknown %s 0x%02x\n", __func__, what, cmd);
    }
}

static void ensure_io_capacity(IPodTouchMultitouchState *s, uint32_t size) {
    if(size > s->io_capacity) {
        s->io_capacity = size;
        s->in_buffer = g_realloc(s->in_buffer, size);
        s->out_storage = g_realloc(s->out_storage, size);
    }
    s->out_buffer = s->out_storage;
}

static void prepare_interface_version_response(IPodTouchMultitouchState *s) {
    memset(s->out_buffer + 1, 0, 15);

    // set the interface version
    s->out_buffer[2] = MT_INTERFACE_VERSION;

    // set the max packet size
    s->out_buffer[3] = (MT_MAX_PACKET_SIZE & 0xFF);
    s->out_buffer[4] = (MT_MAX_PACKET_SIZE >> 8) & 0xFF;

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

static void prepare_cmd_status_response(IPodTouchMultitouchState *s) {
    memset(s->out_buffer + 1, 0, 15);

    // TODO we should probably set some CMD status here

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

static void prepare_report_info_response(IPodTouchMultitouchState *s, uint8_t report_id) {
    memset(s->out_buffer + 1, 0, 15);

    // set the error
    s->out_buffer[2] = 0;

    // set the report length
    uint32_t report_length = 0;
    if(report_id == MT_REPORT_UNKNOWN1) {
        report_length = MT_REPORT_UNKNOWN1_SIZE;
    }
    else if(report_id == MT_REPORT_FAMILY_ID) {
        report_length = MT_REPORT_FAMILY_ID_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_INFO) {
        report_length = MT_REPORT_SENSOR_INFO_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_DESC) {
        report_length = MT_REPORT_SENSOR_REGION_DESC_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_PARAM) {
        report_length = MT_REPORT_SENSOR_REGION_PARAM_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_DIMENSIONS) {
        report_length = MT_REPORT_SENSOR_DIMENSIONS_SIZE;
    }
    else {
        log_unknown_command(s, report_id, "report ID");
    }

    s->out_buffer[3] = (report_length & 0xFF);
    s->out_buffer[4] = (report_length >> 8) & 0xFF;

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

static void prepare_short_control_response(IPodTouchMultitouchState *s, uint8_t report_id) {
    memset(s->out_buffer + 1, 0, 15);

    if(report_id == MT_REPORT_FAMILY_ID) {
        s->out_buffer[3] = MT_FAMILY_ID;
    }
    else if(report_id == MT_REPORT_SENSOR_INFO) {
        s->out_buffer[3] = MT_ENDIANNESS;
        s->out_buffer[4] = MT_SENSOR_ROWS;
        s->out_buffer[5] = MT_SENSOR_COLUMNS;
        s->out_buffer[6] = (MT_BCD_VERSION & 0xFF);
        s->out_buffer[7] = (MT_BCD_VERSION >> 8) & 0xFF;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_DESC) {
        s->out_buffer[3] = MT_SENSOR_REGION_DESC;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_PARAM) {
        s->out_buffer[3] = MT_SENSOR_REGION_PARAM;
    }
    else if(report_id == MT_REPORT_SENSOR_DIMENSIONS) {
        uint32_t *ob_int32 = (uint32_t *)&s->out_buffer[3];
        ob_int32[0] = MT_SENSOR_SURFACE_WIDTH;
        ob_int32[1] = MT_SENSOR_SURFACE_HEIGHT;
    }
    else {
        log_unknown_command(s, report_id, "report ID");
    }

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

// The length packet of a frame read when there's no frame: a zero length tells the driver there's nothing to read.
static void prepare_empty_frame_length_response(IPodTouchMultitouchState *s) {
    memset(s->out_buffer, 0, sizeof(MTFrameLengthPacket));
    s->out_buffer[0] = MT_CMD_FRAME_READ;
    s->out_buffer[14] = MT_CMD_FRAME_READ;
}

// Forget the command in progress, e.g., when the chip select is deasserted in the middle of a command.
static void reset_command_state(IPodTouchMultitouchState *s) {
    s->cur_cmd = 0;
    s->buf_size = 0;
    s->buf_ind = 0;
    s->in_buffer_ind = 0;
    s->frame_data_read = false;
}

static void drop_touch_state(IPodTouchMultitouchState *s) {
    timer_del(s->touch_timer);
    s->frame_pending = false;
    s->press_latched = false;
    s->reported_down = false;
    s->end_pending = false;
}

// The driver bootloads the controller after powering it up or resetting it, and only reads frames once the firmware
// runs. Frames sent earlier make the driver fail to read them and reset the controller again.
static void set_firmware_running(IPodTouchMultitouchState *s, bool running) {
    if(s->firmware_running != running) {
        s->firmware_running = running;
        if(!running) {
            drop_touch_state(s);
        }
    }
}

static uint32_t ipod_touch_multitouch_transfer(SSIPeripheral *dev, uint32_t value)
{
    IPodTouchMultitouchState *s = IPOD_TOUCH_MULTITOUCH(dev);

    //printf("<MULTITOUCH> Got value: 0x%02x\n", value);

    if(s->cur_cmd == 0) {
        //printf("Starting command 0x%02x\n", value);
        // we're currently not in a command - start a new command
        s->cur_cmd = value;
        ensure_io_capacity(s, MT_IO_BUFFER_SIZE);
        s->out_buffer[0] = value; // the response header
        s->buf_ind = 0;
        s->in_buffer_ind = 0;
        
        if(value == 0x00) {
            // The driver clears the interrupt by clocking out the first two bytes of its transmit buffer, which is
            // all zeros after a controller reset.
            s->buf_size = 1;
        }
        else if(value == 0x18) { // filler packet??
            s->buf_size = 2;
            s->out_buffer[1] = 0xE1;
        }
        else if(value == 0x1A) { // HBPP ACK
            set_firmware_running(s, false);
            s->buf_size = 2;
            if(s->hbpp_atn_ack_response[0] == 0 && s->hbpp_atn_ack_response[1] == 0) {
                // return the default ACK response
                s->out_buffer[0] = 0x4B;
                s->out_buffer[1] = 0xC1;
            }
            else {
                s->out_buffer[0] = s->hbpp_atn_ack_response[0];
                s->out_buffer[1] = s->hbpp_atn_ack_response[1];
            }
             
        }
        else if(value == 0x1C) { // read register
            s->buf_size = 8;
            memset(s->out_buffer, 0, 8); // just return zeros
        }
        else if(value == 0x1D) { // execute
            s->buf_size = 12;
            memset(s->out_buffer, 0, 12); // just return zeros
        }
        else if(value == 0x1F) { // calibration
            s->buf_size = 2;
            s->out_buffer[1] = 0x0;
        }
        else if(value == 0x1E) { // write register
            s->buf_size = 16;
            memset(s->out_buffer, 0, 16); // just return zeros
        }
        else if(value == MT_CMD_HBPP_DATA_PACKET) {
            set_firmware_running(s, false);
            s->buf_size = 20; // should be enough initially, until we get the packet length
            memset(s->out_buffer + 1, 0, 20 - 1); // just return zeros
        }
        else if(value == 0x47) { // unknown command, probably used to clear the interrupt
            s->buf_size = 2;
        }
        else if(value == MT_CMD_GET_CMD_STATUS) {
            s->buf_size = 16;
            prepare_cmd_status_response(s);
        }
        else if(value == MT_CMD_GET_INTERFACE_VERSION) {
            // the first thing the driver does after the bootload, before reading the device properties
            set_firmware_running(s, true);
            s->buf_size = 16;
            prepare_interface_version_response(s);
        }
        else if(value == MT_CMD_GET_REPORT_INFO) {
            s->buf_size = 16;
        }
        else if(value == MT_CMD_SHORT_CONTROL_WRITE) {
            s->buf_size = 16;
        }
        else if(value == MT_CMD_SHORT_CONTROL_READ) {
            s->buf_size = 16;
        }
        else if(value == MT_CMD_FRAME_READ || value == MT_CMD_FRAME_READ_FLIP) {
            // Both the frame length and the frame data are read with this command, the third byte tells them apart.
            // Start with the length packet, the frame packet starts with the same two bytes.
            s->buf_size = sizeof(MTFrameLengthPacket);
            if(s->frame_pending) {
                s->out_buffer = (uint8_t *) &s->frame.frame_length;
            }
            else {
                prepare_empty_frame_length_response(s);
            }
        }
        else {
            log_unknown_command(s, value, "command");
            s->buf_size = 1;
            s->out_buffer[0] = 0;
        }
    }

    if(s->in_buffer_ind < s->io_capacity) {
        s->in_buffer[s->in_buffer_ind] = value;
    }
    s->in_buffer_ind++;

    if(s->cur_cmd == MT_CMD_HBPP_DATA_PACKET && s->in_buffer_ind == 10) {
        // verify the header checksum
        uint32_t checksum = 0;
        for(int i = 2; i < 8; i++) {
            checksum += s->in_buffer[i];
        }

        if(checksum != (s->in_buffer[8] << 8 | s->in_buffer[9])) {
            hw_error("HBPP data header checksum doesn't match!");
        }

        uint32_t data_len = (s->in_buffer[2] << 10) | (s->in_buffer[3] << 2) + 5;
        // extend the lengths of the in/out buffers
        ensure_io_capacity(s, data_len + 0x10);
        memset(s->out_buffer, 0, data_len);
        s->buf_size = data_len;
        s->buf_ind = 0;
    }
    else if(s->cur_cmd == MT_CMD_GET_REPORT_INFO && s->in_buffer_ind == 2) {
        prepare_report_info_response(s, s->in_buffer[1]);
    }
    else if(s->cur_cmd == MT_CMD_SHORT_CONTROL_WRITE && s->in_buffer_ind == 16) {
        // TODO we should persist the report here!
    }
    else if(s->cur_cmd == MT_CMD_SHORT_CONTROL_READ && s->in_buffer_ind == 2) {
        prepare_short_control_response(s, s->in_buffer[1]);
    }
    else if((s->cur_cmd == MT_CMD_FRAME_READ || s->cur_cmd == MT_CMD_FRAME_READ_FLIP) && s->in_buffer_ind == 3 &&
            s->in_buffer[2] == 1 && s->frame_pending) {
        // the driver reads the frame data (the length it got before + 5 bytes)
        s->out_buffer = (uint8_t *) &s->frame.frame_packet;
        s->buf_size = sizeof(MTFrame) - sizeof(MTFrameLengthPacket);
        s->frame_data_read = true;
    }

    // TODO process register writes!

    uint8_t ret_val = s->buf_ind < s->buf_size ? s->out_buffer[s->buf_ind] : 0;
    s->buf_ind++;

    //printf("<MULTITOUCH> Got value: 0x%02x, returning 0x%02x (index: %d, buffer length: %d)\n", value, ret_val, s->buf_ind, s->buf_size);

    if(s->buf_ind >= s->buf_size) {
        //printf("Finished command 0x%02x\n", s->cur_cmd);

        if(s->frame_data_read) {
            s->frame_pending = false;
        }

        if(s->cur_cmd == 0x1E) {
            // make sure we return a success status on the next HBPP ACK
            s->hbpp_atn_ack_response[0] = 0x4A;
            s->hbpp_atn_ack_response[1] = 0xD1;
        }

        // we're done with the command
        reset_command_state(s);
    }

    return ret_val;
}

// Commands are framed by the chip select line, which the driver drives through a GPIO. Deselecting the controller
// aborts an unfinished command, e.g., the two-byte transfer the driver uses to clear the interrupt.
static int ipod_touch_multitouch_set_cs(SSIPeripheral *dev, bool level)
{
    IPodTouchMultitouchState *s = IPOD_TOUCH_MULTITOUCH(dev);
    if(level) {
        reset_command_state(s);
    }
    return 0;
}

// The driver resets the controller (and then bootloads it again) through a GPIO, e.g., after too many errors.
static void ipod_touch_multitouch_reset_line(void *opaque, int n, int level)
{
    IPodTouchMultitouchState *s = IPOD_TOUCH_MULTITOUCH(opaque);
    if(level == s->reset_level) {
        return;
    }
    s->reset_level = level;
    reset_command_state(s);
    memset(s->hbpp_atn_ack_response, 0, 2);
    set_firmware_running(s, false);
}

static void build_frame(IPodTouchMultitouchState *s, uint8_t event, float x, float y, uint16_t radius1, uint16_t radius2, uint16_t radius3, uint16_t contactDensity) {
    MTFrame *frame = &s->frame;
    memset(frame, 0, sizeof(MTFrame));

    uint16_t data_len = sizeof(MTFrameHeader) + sizeof(FingerData) + 2;

    /// create the frame length packet
    frame->frame_length.cmd = MT_CMD_FRAME_READ;
    frame->frame_length.length1 = (data_len & 0xFF);
    frame->frame_length.length2 = (data_len >> 8) & 0xFF;

    uint16_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += ((uint8_t *) &frame->frame_length)[i];
    }
    frame->frame_length.checksum1 = (checksum & 0xFF);
    frame->frame_length.checksum2 = (checksum >> 8) & 0xFF;

    // create the frame packet
    // The driver reads the length packet and the frame packet with the same command and we only know which one it
    // wants after we've sent the first two bytes, so both packets start with the command and the length LSB.
    frame->frame_packet.cmd = MT_CMD_FRAME_READ;
    frame->frame_packet.length_lsb = (data_len & 0xFF);
    frame->frame_packet.length1 = (data_len & 0xFF);
    frame->frame_packet.length2 = (data_len >> 8) & 0xFF;

    checksum = 0;
    for(int i = 0; i < 4; i++) {
        checksum += ((uint8_t *) &frame->frame_packet)[i];
    }

    // the first five bytes have to sum up to 0.
    frame->frame_packet.checksum_pad = 0xFF - (checksum & 0xFF) + 1;

    frame->frame_packet.header.type = MT_FRAME_TYPE_PATH;
    frame->frame_packet.header.frameNum = s->frame_counter;
    frame->frame_packet.header.headerLen = sizeof(MTFrameHeader);
    uint32_t timestamp_ms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_MS;
    frame->frame_packet.header.timestamp = timestamp_ms;
    frame->frame_packet.header.numFingers = 1;
    frame->frame_packet.header.fingerDataLen = sizeof(FingerData);

    // create the finger data
    frame->finger_data.id = 1;
    frame->finger_data.event = event;
    frame->finger_data.unk_2 = 2;
    frame->finger_data.unk_3 = 1;

    // MultitouchSupport normalises the position as (pos - min) / (max - min),
    // with the position in 1/100 mm.
    int16_t pos_x = lroundf(MT_SURFACE_X_MIN + x * (MT_SURFACE_X_MAX - MT_SURFACE_X_MIN));
    int16_t pos_y = lroundf(MT_SURFACE_Y_MIN + y * (MT_SURFACE_Y_MAX - MT_SURFACE_Y_MIN) - MT_FINGER_TIP_OFFSET);

    // the velocity is in 1/8 mm/s
    int32_t vel_x = 0, vel_y = 0;
    if(event == MT_EVENT_TOUCH_MOVED || event == MT_EVENT_TOUCH_ENDED) {
        uint32_t dt_ms = MAX(timestamp_ms - s->last_frame_timestamp, 1);
        vel_x = (pos_x - s->last_frame_x) * 80 / (int32_t) dt_ms;
        vel_y = (pos_y - s->last_frame_y) * 80 / (int32_t) dt_ms;
    }
    frame->finger_data.velX = MIN(MAX(vel_x, INT16_MIN), INT16_MAX);
    frame->finger_data.velY = MIN(MAX(vel_y, INT16_MIN), INT16_MAX);

    frame->finger_data.x = pos_x;
    frame->finger_data.y = pos_y;
    frame->finger_data.radius1 = radius1;
    frame->finger_data.radius2 = radius2;
    frame->finger_data.radius3 = radius3;
    frame->finger_data.angle = 19317;
    frame->finger_data.contactDensity = contactDensity; // seems to be a medium press

    // compute the checksum over the frame data.
    checksum = 0;
    for(int i = 0; i < data_len - 2; i++) {
        checksum += ((uint8_t *) &frame->frame_packet.header)[i];
    }
    frame->checksum1 = (checksum & 0xFF);
    frame->checksum2 = (checksum >> 8) & 0xFF;

    s->last_frame_timestamp = timestamp_ms;
    s->last_frame_x = pos_x;
    s->last_frame_y = pos_y;
    s->frame_counter += 1;
}

static void ipod_touch_multitouch_inform_frame_ready(IPodTouchMultitouchState *s) {
    qemu_irq_pulse(s->irq);
}

static void send_frame(IPodTouchMultitouchState *s, uint8_t event) {
    if(event == MT_EVENT_TOUCH_START || event == MT_EVENT_TOUCH_MOVED) {
        build_frame(s, event, s->touch_x, s->touch_y, 100, 660, 580, 150);
    }
    else {
        build_frame(s, event, s->touch_x, s->touch_y, 0, 0, 0, 0);
    }
    s->frame_pending = true;
    s->frame_sent_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ipod_touch_multitouch_inform_frame_ready(s);
}

// All frames are sent from this timer. We only send a new frame after the driver has read the
// previous one: overwriting a frame while it's being read breaks its checksum, and we'd lose the
// touch start/end events if they were replaced before the driver saw them.
static void touch_timer_tick(void *opaque)
{
    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;
    int64_t next = MT_FRAME_INTERVAL_NS;

    if(!s->firmware_running) {
        drop_touch_state(s);
        return;
    }

    if(s->frame_pending) {
        // The driver hasn't read the previous frame yet. If it's been a while and no read is in
        // progress, the driver probably missed the interrupt (e.g., across a device reset).
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if(s->cur_cmd != MT_CMD_FRAME_READ && s->cur_cmd != MT_CMD_FRAME_READ_FLIP && now - s->frame_sent_ns >= MT_FRAME_TIMEOUT_NS) {
            printf("%s: frame %d not read by the driver, raising the interrupt again\n", __func__, s->frame.frame_packet.header.frameNum);
            s->frame_sent_ns = now;
            ipod_touch_multitouch_inform_frame_ready(s);
        }
    }
    else if(s->press_latched) {
        s->press_latched = false;
        s->reported_down = true;
        s->end_pending = false;
        send_frame(s, MT_EVENT_TOUCH_START);
    }
    else if(s->reported_down && s->touch_down) {
        send_frame(s, MT_EVENT_TOUCH_MOVED);
    }
    else if(s->reported_down) {
        s->reported_down = false;
        s->end_pending = true;
        send_frame(s, MT_EVENT_TOUCH_ENDED);
        next = NANOSECONDS_PER_SECOND / 10;
    }
    else if(s->end_pending) {
        s->end_pending = false;
        send_frame(s, MT_EVENT_TOUCH_FULL_END);
        return;
    }
    else {
        return;
    }

    timer_mod(s->touch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + next);
}

void ipod_touch_multitouch_on_touch(IPodTouchMultitouchState *s) {
    s->touch_down = true;
    if(!s->firmware_running) {
        return; // the driver isn't ready for frames yet
    }
    s->press_latched = true; // make sure a quick tap still produces a touch start
    timer_mod(s->touch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

void ipod_touch_multitouch_on_release(IPodTouchMultitouchState *s) {
    s->touch_down = false;
    if(!timer_pending(s->touch_timer)) {
        timer_mod(s->touch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void ipod_touch_multitouch_realize(SSIPeripheral *d, Error **errp)
{
    IPodTouchMultitouchState *s = IPOD_TOUCH_MULTITOUCH(d);
    memset(s->hbpp_atn_ack_response, 0, 2);
    s->touch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, touch_timer_tick, s);
    qdev_init_gpio_out_named(DEVICE(d), &s->irq, "irq", 1);
    qdev_init_gpio_in_named(DEVICE(d), ipod_touch_multitouch_reset_line, "reset", 1);

    ensure_io_capacity(s, MT_IO_BUFFER_SIZE);
}

static void ipod_touch_multitouch_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = ipod_touch_multitouch_realize;
    k->transfer = ipod_touch_multitouch_transfer;
    k->set_cs = ipod_touch_multitouch_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo ipod_touch_multitouch_type_info = {
    .name = TYPE_IPOD_TOUCH_MULTITOUCH,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IPodTouchMultitouchState),
    .class_init = ipod_touch_multitouch_class_init,
};

static void ipod_touch_multitouch_register_types(void)
{
    type_register_static(&ipod_touch_multitouch_type_info);
}

type_init(ipod_touch_multitouch_register_types)
