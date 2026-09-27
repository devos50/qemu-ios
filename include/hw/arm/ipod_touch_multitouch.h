#ifndef IPOD_TOUCH_MULTITOUCH_H
#define IPOD_TOUCH_MULTITOUCH_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "hw/arm/ipod_touch_sysic.h"
#include "hw/arm/ipod_touch_gpio.h"

#define TYPE_IPOD_TOUCH_MULTITOUCH                "ipodtouch.multitouch"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMultitouchState, IPOD_TOUCH_MULTITOUCH)

#define MT_INTERFACE_VERSION     0x1
#define MT_FAMILY_ID             81
#define MT_ENDIANNESS            0x1
#define MT_SENSOR_ROWS           15
#define MT_SENSOR_COLUMNS        10
#define MT_BCD_VERSION           51
#define MT_SENSOR_REGION_DESC    0x0
#define MT_SENSOR_REGION_PARAM   0x0
#define MT_MAX_PACKET_SIZE       0x294 // 660
#define MT_SENSOR_SURFACE_WIDTH  5000
#define MT_SENSOR_SURFACE_HEIGHT 7500

// Position bounds (in 1/100 mm) that MultitouchSupport uses to normalise the
// finger coordinates, see _alg_InitRowColXYConvert. The surface spans from the
// first to the last sensor row/column (pitch 36/7 mm and 56/11 mm for this
// family), extended by a 0.75 mm edge on each side.
#define MT_SURFACE_EDGE   75
#define MT_SURFACE_X_MIN  (-MT_SURFACE_EDGE)
#define MT_SURFACE_X_MAX  ((MT_SENSOR_COLUMNS - 1) * 5600 / 11 + MT_SURFACE_EDGE)
#define MT_SURFACE_Y_MIN  (-MT_SURFACE_EDGE)
#define MT_SURFACE_Y_MAX  ((MT_SENSOR_ROWS - 1) * 3600 / 7 + MT_SURFACE_EDGE)

// MultitouchHID moves every touch up by FingerTipVerticalOffset (SpringBoard sets it to
// 3.5 pt = 1.235 mm, on a screen it treats as 75 mm tall), because the point a finger aims at
// lies above the centre of its contact area. A mouse click has no such offset, so we report the
// contact lower by the same distance, in surface units.
#define MT_FINGER_TIP_OFFSET ((MT_SURFACE_Y_MAX - MT_SURFACE_Y_MIN) * 1.235f / 75.0f)

// how often we send a frame while a finger is on the surface
#define MT_FRAME_INTERVAL_NS (NANOSECONDS_PER_SECOND / 60)
// raise the interrupt again if the driver hasn't read a frame after this long
#define MT_FRAME_TIMEOUT_NS  (NANOSECONDS_PER_SECOND / 10)

// report IDs
#define MT_REPORT_UNKNOWN1            0x70
#define MT_REPORT_FAMILY_ID           0xD1
#define MT_REPORT_SENSOR_INFO         0xD3
#define MT_REPORT_SENSOR_REGION_DESC  0xD0
#define MT_REPORT_SENSOR_REGION_PARAM 0xA1
#define MT_REPORT_SENSOR_DIMENSIONS   0xD9

// report sizes
#define MT_REPORT_UNKNOWN1_SIZE            0x1
#define MT_REPORT_FAMILY_ID_SIZE           0x1
#define MT_REPORT_SENSOR_INFO_SIZE         0x5
#define MT_REPORT_SENSOR_REGION_DESC_SIZE  0x1
#define MT_REPORT_SENSOR_REGION_PARAM_SIZE 0x1
#define MT_REPORT_SENSOR_DIMENSIONS_SIZE   0x8

#define MT_CMD_HBPP_DATA_PACKET      0x30
#define MT_CMD_GET_CMD_STATUS        0xE1
#define MT_CMD_GET_INTERFACE_VERSION 0xE2
#define MT_CMD_GET_REPORT_INFO       0xE3
#define MT_CMD_SHORT_CONTROL_WRITE   0xE4
#define MT_CMD_SHORT_CONTROL_READ    0xE6
#define MT_CMD_FRAME_READ            0xEA

// frame types
#define MT_FRAME_TYPE_PATH 0x44

// frame event types
#define MT_EVENT_TOUCH_FULL_END 0x0
#define MT_EVENT_TOUCH_START 0x3
#define MT_EVENT_TOUCH_MOVED 0x4
#define MT_EVENT_TOUCH_ENDED 0x7

typedef struct MTFrameLengthPacket
{
    uint8_t cmd;
    uint8_t length1;
    uint8_t length2;
    uint8_t unused[11];
    uint8_t checksum1;
    uint8_t checksum2;
} __attribute__((__packed__)) MTFrameLengthPacket;

typedef struct MTFrameHeader
{
    uint8_t type;
    uint8_t frameNum;
    uint8_t headerLen;
    uint8_t unk_3;
    uint32_t timestamp;
    uint8_t unk_8;
    uint8_t unk_9;
    uint8_t unk_A;
    uint8_t unk_B;
    uint16_t unk_C;
    uint16_t isImage;

    uint8_t numFingers;
    uint8_t fingerDataLen;
    uint16_t unk_12;
    uint16_t unk_14;
    uint16_t unk_16;
} __attribute__((__packed__)) MTFrameHeader;

typedef struct MTFramePacket
{
    uint8_t cmd;
    uint8_t unused1;
    uint8_t length1;
    uint8_t length2;
    uint8_t checksum_pad;
    MTFrameHeader header;
} __attribute__((__packed__)) MTFramePacket;

typedef struct FingerData
{
    uint8_t id;
    uint8_t event;
    uint8_t unk_2;
    uint8_t unk_3;
    int16_t x;
    int16_t y;
    int16_t velX;
    int16_t velY;
    uint16_t radius2;
    uint16_t radius3;
    uint16_t angle;
    uint16_t radius1;
    uint16_t contactDensity;
    uint16_t unk_16;
    uint16_t unk_18;
    uint16_t unk_1A;
} __attribute__((__packed__)) FingerData;

typedef struct MTFrame {
    MTFrameLengthPacket frame_length;
    MTFramePacket frame_packet;
    FingerData finger_data; // TODO we assume one finger for now
    uint8_t checksum1;
    uint8_t checksum2;
} __attribute__((__packed__)) MTFrame;

typedef struct IPodTouchMultitouchState {
    SSIPeripheral ssidev;
    uint8_t cur_cmd;
    uint8_t *out_buffer;
    uint8_t *in_buffer;
    uint8_t *out_storage;
    uint32_t io_capacity;
    uint32_t buf_size;
    uint32_t buf_ind;
    uint32_t in_buffer_ind;
    uint8_t hbpp_atn_ack_response[2];
    MTFrame frame;
    bool frame_pending; // the driver hasn't read the frame yet
    int64_t frame_sent_ns;
    uint32_t frame_counter;
    bool touch_down;     // host button state
    bool press_latched;  // a press that hasn't been reported yet
    bool reported_down;  // the driver has seen a touch start
    bool end_pending;    // a touch end still has to be followed by a full end
    QEMUTimer *touch_timer;
    IPodTouchSYSICState *sysic;
    IPodTouchGPIOState *gpio_state;
    float touch_x;
    float touch_y;
    int16_t last_frame_x;
    int16_t last_frame_y;
    uint32_t last_frame_timestamp;
} IPodTouchMultitouchState;

void ipod_touch_multitouch_on_touch(IPodTouchMultitouchState *s);
void ipod_touch_multitouch_on_release(IPodTouchMultitouchState *s);

#endif