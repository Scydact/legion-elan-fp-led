/*
 * elan-led-shim: LD_PRELOAD library for fprintd that lights the Lenovo Legion
 * power-button LED green while the ELAN 04f3:0c4b fingerprint sensor waits
 * for a finger, the same way the Windows driver does.
 *
 * The LED is driven by the sensor itself through vendor register writes
 * (see docs/protocol.md). fprintd holds the USB device during a scan, so the
 * commands have to be sent from inside fprintd: this library wraps
 * libusb_submit_transfer() and friends and watches the bulk OUT commands the
 * driver sends on endpoint 0x01 of the ELAN device:
 *
 *   40 3f  (wait for finger)  -> send "LED green" just before it
 *   00 0b  (stop)             -> send "LED restore" just after it
 *   release / close           -> send "LED restore" if still green
 *
 * Environment:
 *   ELAN_LED_MODE=inject  send the LED commands (default)
 *   ELAN_LED_MODE=log     log every command to stderr, send nothing
 *   ELAN_LED_MODE=off     do nothing
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define ELAN_VID 0x04f3
#define ELAN_PID 0x0c4b
#define EP_CMD   0x01
#define CMD_LEN  9

enum mode { MODE_UNSET = -1, MODE_OFF, MODE_LOG, MODE_INJECT };

static const unsigned char LED_GREEN[2][CMD_LEN] = {
    {0x46, 0x07, 0x90, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00},
    {0x46, 0x07, 0x80, 0x01, 0x04, 0x40, 0x00, 0x00, 0x00},
};
static const unsigned char LED_RESTORE[2][CMD_LEN] = {
    {0x46, 0x07, 0x80, 0x01, 0x04, 0x40, 0x00, 0x00, 0x00},
    {0x46, 0x07, 0x90, 0x01, 0x04, 0x10, 0x00, 0x00, 0x00},
};

static int (*real_submit)(struct libusb_transfer *);
static int (*real_bulk)(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned int);
static int (*real_release)(libusb_device_handle *, int);
static void (*real_close)(libusb_device_handle *);

static enum mode mode = MODE_UNSET;
static int led_is_green;

static void init(void)
{
    if (mode != MODE_UNSET)
        return;
    real_submit  = dlsym(RTLD_NEXT, "libusb_submit_transfer");
    real_bulk    = dlsym(RTLD_NEXT, "libusb_bulk_transfer");
    real_release = dlsym(RTLD_NEXT, "libusb_release_interface");
    real_close   = dlsym(RTLD_NEXT, "libusb_close");

    const char *m = getenv("ELAN_LED_MODE");
    if (m && strcmp(m, "off") == 0)
        mode = MODE_OFF;
    else if (m && strcmp(m, "log") == 0)
        mode = MODE_LOG;
    else
        mode = MODE_INJECT;
    fprintf(stderr, "elan-led-shim: loaded, mode=%s\n",
            mode == MODE_OFF ? "off" : mode == MODE_LOG ? "log" : "inject");
}

static int is_elan(libusb_device_handle *h)
{
    struct libusb_device_descriptor d;
    if (mode == MODE_OFF || !h || libusb_get_device_descriptor(libusb_get_device(h), &d) != 0)
        return 0;
    return d.idVendor == ELAN_VID && d.idProduct == ELAN_PID;
}

static int is_cmd(const unsigned char *b, int len, unsigned char c0, unsigned char c1)
{
    return b && len >= 2 && b[0] == c0 && b[1] == c1;
}

static void log_cmd(const char *via, const unsigned char *b, int len)
{
    char hex[40] = "";
    if (mode != MODE_LOG)
        return;
    for (int i = 0; i < len && i < 16; i++)
        snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", b[i]);
    fprintf(stderr, "elan-led-shim: %s OUT %s%s\n", via, hex, len > 16 ? "..." : "");
}

static void noop_cb(struct libusb_transfer *t) { (void)t; }

/* Queue a command asynchronously; the driver may be inside the libusb event loop. */
static void send_async(libusb_device_handle *h, const unsigned char *cmd)
{
    struct libusb_transfer *t = libusb_alloc_transfer(0);
    unsigned char *buf = malloc(CMD_LEN);
    if (!t || !buf) {
        free(buf);
        libusb_free_transfer(t);
        return;
    }
    memcpy(buf, cmd, CMD_LEN);
    libusb_fill_bulk_transfer(t, h, EP_CMD, buf, CMD_LEN, noop_cb, NULL, 1000);
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
    if (real_submit(t) != 0)
        libusb_free_transfer(t);
}

static void send_sync(libusb_device_handle *h, const unsigned char *cmd)
{
    int done;
    real_bulk(h, EP_CMD, (unsigned char *)cmd, CMD_LEN, &done, 1000);
}

static void set_led(libusb_device_handle *h, int green, int async)
{
    if (mode != MODE_INJECT || green == led_is_green)
        return;
    const unsigned char (*seq)[CMD_LEN] = green ? LED_GREEN : LED_RESTORE;
    for (int i = 0; i < 2; i++) {
        if (async)
            send_async(h, seq[i]);
        else
            send_sync(h, seq[i]);
    }
    led_is_green = green;
}

int libusb_submit_transfer(struct libusb_transfer *t)
{
    init();
    if (t->type != LIBUSB_TRANSFER_TYPE_BULK || t->endpoint != EP_CMD || !is_elan(t->dev_handle))
        return real_submit(t);
    log_cmd("async", t->buffer, t->length);
    if (is_cmd(t->buffer, t->length, 0x40, 0x3f))
        set_led(t->dev_handle, 1, 1);
    int r = real_submit(t);
    if (is_cmd(t->buffer, t->length, 0x00, 0x0b))
        set_led(t->dev_handle, 0, 1);
    return r;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
                         int len, int *transferred, unsigned int timeout)
{
    init();
    if (ep != EP_CMD || !is_elan(h))
        return real_bulk(h, ep, data, len, transferred, timeout);
    log_cmd("sync", data, len);
    if (is_cmd(data, len, 0x40, 0x3f))
        set_led(h, 1, 0);
    int r = real_bulk(h, ep, data, len, transferred, timeout);
    if (is_cmd(data, len, 0x00, 0x0b))
        set_led(h, 0, 0);
    return r;
}

int libusb_release_interface(libusb_device_handle *h, int iface)
{
    init();
    if (is_elan(h))
        set_led(h, 0, 0);
    return real_release(h, iface);
}

void libusb_close(libusb_device_handle *h)
{
    init();
    if (is_elan(h))
        set_led(h, 0, 0);
    real_close(h);
}
