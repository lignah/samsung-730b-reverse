/*
 * Samsung 730B fingerprint sensor driver for libfprint
 * Copyright (C) 2025 lignah
 */

#define FP_COMPONENT "samsung730b"

#include "drivers_api.h"
#include <stdio.h>

#define EP_IN   0x82
#define EP_OUT  0x01

#define BULK_TIMEOUT 500
#define CTRL_TIMEOUT 1000

#define IMG_WIDTH  224
#define IMG_HEIGHT 192
#define RAW_WIDTH  112
#define RAW_HEIGHT 96
#define IMG_WIDTH  224
#define IMG_HEIGHT 192
#define IMG_SIZE   (IMG_WIDTH * IMG_HEIGHT)

#define RAW_DATA_SIZE 21760
#define CHUNK_SIZE 256
#define NUM_CHUNKS 85

struct _FpiDeviceSamsung730b
{
    FpImageDevice parent;
    guint8       *buffer;
    gsize         buffer_offset;
};

G_DECLARE_FINAL_TYPE(FpiDeviceSamsung730b, fpi_device_samsung730b, FPI, DEVICE_SAMSUNG730B, FpImageDevice)
G_DEFINE_TYPE(FpiDeviceSamsung730b, fpi_device_samsung730b, FP_TYPE_IMAGE_DEVICE)

enum {
    M_CAPTURE,
    M_SUBMIT_IMAGE,
    M_NUM_STATES,
};

static void do_init_sequence(GUsbDevice *usb_dev) {
    gsize actual_len;
    guint8 buf[256];
    guint8 resp[256];

    /* Control init */
    guint8 ctrl_data[] = {
        0x80, 0x84, 0x1e, 0x00, 0x08, 0x00, 0x00, 0x01,
        0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    g_usb_device_control_transfer(usb_dev,
        G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
        G_USB_DEVICE_REQUEST_TYPE_VENDOR,
        G_USB_DEVICE_RECIPIENT_DEVICE,
        0xC3, 0x0000, 0x0000,
        ctrl_data, sizeof(ctrl_data), &actual_len,
        CTRL_TIMEOUT, NULL, NULL);

    /* Full init sequence */
    static const guint8 init_cmds[][4] = {
        {0x4f, 0x80, 0x00, 0x00},
        {0xa9, 0x4f, 0x80, 0x00},
        {0xa8, 0xb9, 0x00, 0x00},
        {0xa9, 0x60, 0x1b, 0x00},
        {0xa9, 0x50, 0x21, 0x00},
        {0xa9, 0x61, 0x00, 0x00},
        {0xa9, 0x62, 0x00, 0x1a},
        {0xa9, 0x63, 0x00, 0x1a},
        {0xa9, 0x64, 0x04, 0x0a},
        {0xa9, 0x66, 0x0f, 0x80},
        {0xa9, 0x67, 0x1b, 0x00},
        {0xa9, 0x68, 0x00, 0x0f},
        {0xa9, 0x69, 0x00, 0x14},
        {0xa9, 0x6a, 0x00, 0x19},
        {0xa9, 0x6c, 0x00, 0x19},
        {0xa9, 0x40, 0x43, 0x00},
        {0xa9, 0x41, 0x6f, 0x00},
        {0xa9, 0x55, 0x20, 0x00},
        {0xa9, 0x5f, 0x00, 0x00},
        {0xa9, 0x52, 0x27, 0x00},
        {0xa9, 0x09, 0x00, 0x00},
        {0xa9, 0x5d, 0x4d, 0x00},
        {0xa9, 0x51, 0xa8, 0x25},
        {0xa9, 0x03, 0x00, 0x00},
        {0xa9, 0x38, 0x01, 0x00},
        {0xa9, 0x3d, 0xff, 0x0f},
        {0xa9, 0x10, 0x60, 0x00},
        {0xa9, 0x3b, 0x14, 0x00},
        {0xa9, 0x2f, 0xf6, 0xff},
        {0xa9, 0x09, 0x00, 0x00},
        {0xa9, 0x0c, 0x00, 0x00},
        {0xa8, 0x20, 0x00, 0x00},
        {0xa9, 0x04, 0x00, 0x00},
        {0xa8, 0x08, 0x00, 0x00},
        {0xa9, 0x09, 0x00, 0x00},
        {0xa8, 0x3e, 0x00, 0x00},
        {0xa9, 0x03, 0x00, 0x00},
        {0xa8, 0x20, 0x00, 0x00},
        {0xa9, 0x10, 0x00, 0x01},
        {0xa9, 0x2f, 0xef, 0x00},
        {0xa9, 0x09, 0x00, 0x00},
        {0xa9, 0x5d, 0x4d, 0x00},
        {0xa9, 0x51, 0x3a, 0x25},
        {0xa9, 0x0c, 0x00, 0x00},
        {0xa8, 0x20, 0x00, 0x00},
        {0xa9, 0x04, 0x00, 0x00},
        {0xa9, 0x09, 0x00, 0x00},
    };
    static const gsize lens[] = {
        2, 3, 3, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 3, 4, 4, 4, 4, 4, 4,
        3, 4, 3, 3, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 3, 4, 4, 4,
    };

    for (gsize i = 0; i < G_N_ELEMENTS(init_cmds); i++) {
        memset(buf, 0, sizeof(buf));
        memcpy(buf, init_cmds[i], lens[i]);
        g_usb_device_bulk_transfer(usb_dev, EP_OUT,
            buf, lens[i], &actual_len, BULK_TIMEOUT, NULL, NULL);
        g_usb_device_bulk_transfer(usb_dev, EP_IN,
            resp, 256, &actual_len, BULK_TIMEOUT, NULL, NULL);
    }
}

static void
ssm_run_state(FpiSsm *ssm, FpDevice *device)
{
    FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B(device);
    FpImageDevice *img_dev = FP_IMAGE_DEVICE(device);
    GUsbDevice *usb_dev = fpi_device_get_usb_device(device);

    fp_dbg("State: %d", fpi_ssm_get_cur_state(ssm));

    switch (fpi_ssm_get_cur_state(ssm)) {
        
    case M_CAPTURE:
        {
            gsize actual_len;
            guint8 buf[256];
            guint8 resp[256];
            guint i;
            FpiDeviceAction action;

            /* Check if action is still valid */
            action = fpi_device_get_current_action(device);
            if (action == FPI_DEVICE_ACTION_NONE) {
                fpi_ssm_mark_completed(ssm);
                return;
            }

            self->buffer = g_malloc0(RAW_DATA_SIZE);
            self->buffer_offset = 0;

            do_init_sequence(usb_dev);
            g_usleep(1000000);
            fpi_image_device_report_finger_status(img_dev, TRUE);

            for (i = 0; i < NUM_CHUNKS; i++) {
                guint16 wIndex = 0x032A + (i * CHUNK_SIZE);

                g_usb_device_control_transfer(usb_dev,
                    G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                    G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                    G_USB_DEVICE_RECIPIENT_DEVICE,
                    0xCA, 0x0003, wIndex,
                    NULL, 0, NULL,
                    CTRL_TIMEOUT, NULL, NULL);

                if (i == 0) {
                    memset(buf, 0, sizeof(buf));
                    buf[0] = 0xa8;
                    buf[1] = 0x06;
                    g_usb_device_bulk_transfer(usb_dev, EP_OUT,
                        buf, 256, &actual_len, BULK_TIMEOUT, NULL, NULL);
                }

                memset(resp, 0, sizeof(resp));
                if (g_usb_device_bulk_transfer(usb_dev, EP_IN,
                        resp, 256, &actual_len, BULK_TIMEOUT, NULL, NULL)) {
                    if (actual_len > 0 && self->buffer_offset < RAW_DATA_SIZE) {
                        gsize to_copy = MIN(actual_len, RAW_DATA_SIZE - self->buffer_offset);
                        memcpy(self->buffer + self->buffer_offset, resp, to_copy);
                        self->buffer_offset += to_copy;
                    }
                } else {
                    fp_dbg("Read failed at chunk %u", i);
                    break;
                }

                memset(buf, 0, sizeof(buf));
                g_usb_device_bulk_transfer(usb_dev, EP_OUT,
                    buf, 256, &actual_len, BULK_TIMEOUT, NULL, NULL);
            }

            fp_dbg("Captured %zu bytes in %u chunks", self->buffer_offset, i);
            fpi_ssm_next_state(ssm);
        }
        break;

    case M_SUBMIT_IMAGE:
        {
            FpImage *img;
            gsize offset = 4;
            FpiDeviceAction action;
            guint x, y;
            guint8 *raw_data;

            action = fpi_device_get_current_action(device);
            if (action == FPI_DEVICE_ACTION_NONE) {
                g_clear_pointer(&self->buffer, g_free);
                fpi_ssm_mark_completed(ssm);
                return;
            }

            fp_dbg("Submitting image, captured %zu bytes", self->buffer_offset);

            img = fp_image_new(IMG_WIDTH, IMG_HEIGHT);
            raw_data = self->buffer + offset;

            /* 2x 업스케일: 112x96 -> 224x192 */
            for (y = 0; y < RAW_HEIGHT; y++) {
                for (x = 0; x < RAW_WIDTH; x++) {
                    guint8 pixel = raw_data[y * RAW_WIDTH + x];
                    guint dst_y = y * 2;
                    guint dst_x = x * 2;
                    
                    img->data[dst_y * IMG_WIDTH + dst_x] = pixel;
                    img->data[dst_y * IMG_WIDTH + dst_x + 1] = pixel;
                    img->data[(dst_y + 1) * IMG_WIDTH + dst_x] = pixel;
                    img->data[(dst_y + 1) * IMG_WIDTH + dst_x + 1] = pixel;
                }
            }

            /* 디버그: 업스케일된 이미지 저장 */
            FILE *f = fopen("/var/tmp/fp_upscaled.raw", "wb");
            if (f) {
                fwrite(img->data, 1, IMG_WIDTH * IMG_HEIGHT, f);
                fclose(f);
            }
            img->ppmm = 19.69;  /* 500 DPI */
            img->flags = FPI_IMAGE_COLORS_INVERTED;

            g_clear_pointer(&self->buffer, g_free);
            fpi_image_device_image_captured(img_dev, img);
            fpi_image_device_report_finger_status(img_dev, FALSE);

            fpi_ssm_mark_completed(ssm);
        }
        break;

    default:
        g_assert_not_reached();
    }
}

static void
ssm_done(FpiSsm *ssm, FpDevice *device, GError *error)
{
    FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B(device);
    FpImageDevice *img_dev = FP_IMAGE_DEVICE(device);
    GUsbDevice *usb_dev = fpi_device_get_usb_device(device);
    gsize actual_len;
    guint8 buf[256] = {0};

    fp_dbg("SSM done, error: %s", error ?  error->message : "none");

    g_clear_pointer(&self->buffer, g_free);

    if (error) {
        fpi_image_device_session_error(img_dev, error);
        return;
    }

    buf[0] = 0xa9; buf[1] = 0x09; buf[2] = 0x00; buf[3] = 0x00;
    g_usb_device_bulk_transfer(usb_dev, EP_OUT, buf, 256, &actual_len, 100, NULL, NULL);
    g_usb_device_bulk_transfer(usb_dev, EP_IN, buf, 256, &actual_len, 100, NULL, NULL);
    
    buf[0] = 0xa8; buf[1] = 0x20; buf[2] = 0x00; buf[3] = 0x00;
    g_usb_device_bulk_transfer(usb_dev, EP_OUT, buf, 256, &actual_len, 100, NULL, NULL);
    g_usb_device_bulk_transfer(usb_dev, EP_IN, buf, 256, &actual_len, 100, NULL, NULL);

    /* 0. 5초 후 action 다시 체크하고 재시작 */
    g_usleep(100000);
    FpiDeviceAction action = fpi_device_get_current_action(device);
    fp_dbg("After sleep, action: %d", action);
    if (action == FPI_DEVICE_ACTION_ENROLL ||
        action == FPI_DEVICE_ACTION_VERIFY ||
        action == FPI_DEVICE_ACTION_IDENTIFY) {
        FpiSsm *new_ssm = fpi_ssm_new(device, ssm_run_state, M_NUM_STATES);
        fpi_ssm_start(new_ssm, ssm_done);
    }
}

static void
dev_open(FpImageDevice *dev)
{
    FpDevice *device = FP_DEVICE(dev);
    GUsbDevice *usb_dev = fpi_device_get_usb_device(device);
    GError *error = NULL;

    fp_dbg("dev_open");

    if (!g_usb_device_claim_interface(usb_dev, 0, 0, &error)) {
        fpi_image_device_open_complete(dev, error);
        return;
    }

    do_init_sequence(usb_dev);

    fpi_image_device_open_complete(dev, NULL);
}

static void
dev_close(FpImageDevice *dev)
{
    GUsbDevice *usb_dev = fpi_device_get_usb_device(FP_DEVICE(dev));
    GError *error = NULL;
    gsize actual_len;
    guint8 buf[256] = {0};

    fp_dbg("dev_close");

    /* Reset sensor */
    buf[0] = 0xa9;
    buf[1] = 0x09;
    g_usb_device_bulk_transfer(usb_dev, EP_OUT,
        buf, 4, &actual_len, BULK_TIMEOUT, NULL, NULL);
    g_usb_device_bulk_transfer(usb_dev, EP_IN,
        buf, 256, &actual_len, BULK_TIMEOUT, NULL, NULL);

    g_usb_device_release_interface(usb_dev, 0, 0, &error);
    fpi_image_device_close_complete(dev, error);
}

static void
dev_activate(FpImageDevice *dev)
{
    FpDevice *device = FP_DEVICE(dev);
    FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B(device);
    GUsbDevice *usb_dev = fpi_device_get_usb_device(device);
    gsize actual_len;
    guint8 buf[256] = {0};
    FpiSsm *ssm;

    fp_dbg("dev_activate");

    /* Reset sensor before capture */
    buf[0] = 0xa9; buf[1] = 0x09; buf[2] = 0x00; buf[3] = 0x00;
    g_usb_device_bulk_transfer(usb_dev, EP_OUT, buf, 256, &actual_len, 100, NULL, NULL);
    g_usb_device_bulk_transfer(usb_dev, EP_IN, buf, 256, &actual_len, 100, NULL, NULL);
    
    buf[0] = 0xa8; buf[1] = 0x20; buf[2] = 0x00; buf[3] = 0x00;
    g_usb_device_bulk_transfer(usb_dev, EP_OUT, buf, 256, &actual_len, 100, NULL, NULL);
    g_usb_device_bulk_transfer(usb_dev, EP_IN, buf, 256, &actual_len, 100, NULL, NULL);

    self->buffer = NULL;
    self->buffer_offset = 0;

    fpi_image_device_activate_complete(dev, NULL);

    ssm = fpi_ssm_new(device, ssm_run_state, M_NUM_STATES);
    fpi_ssm_start(ssm, ssm_done);
}

static void
dev_deactivate(FpImageDevice *dev)
{
    FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B(dev);
    fp_dbg("dev_deactivate");
    g_clear_pointer(&self->buffer, g_free);
    fpi_image_device_deactivate_complete(dev, NULL);
}

static const FpIdEntry id_table[] = {
    { . vid = 0x04e8, .pid = 0x730b },
    { .vid = 0, .pid = 0 },
};

static void
fpi_device_samsung730b_class_init(FpiDeviceSamsung730bClass *klass)
{
    FpDeviceClass *dev_class = FP_DEVICE_CLASS(klass);
    FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS(klass);

    dev_class->id = "samsung730b";
    dev_class->full_name = "Samsung 730B Fingerprint Sensor";
    dev_class->type = FP_DEVICE_TYPE_USB;
    dev_class->id_table = id_table;
    dev_class->scan_type = FP_SCAN_TYPE_PRESS;

    img_class->img_width = IMG_WIDTH;
    img_class->img_height = IMG_HEIGHT;
    img_class->bz3_threshold = 12;

    img_class->img_open = dev_open;
    img_class->img_close = dev_close;
    img_class->activate = dev_activate;
    img_class->deactivate = dev_deactivate;
}

static void
fpi_device_samsung730b_init(FpiDeviceSamsung730b *self)
{
}