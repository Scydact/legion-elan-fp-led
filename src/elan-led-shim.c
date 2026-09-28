/*
 * elan-led-shim: LD_PRELOAD library for fprintd that lights the Legion
 * power-button LED green while the ELAN 04f3:0c4b sensor waits for a finger.
 *
 * It watches bulk OUT commands on endpoint 0x01 of the ELAN device:
 *   40 3f (wait for finger)  -> first send the green commands
 *   00 0b (stop)             -> afterwards send the restore commands
 *   release/close            -> send the restore commands
 *
 * ELAN_LED_MODE=log     only log commands to stderr (journal), send nothing
 * ELAN_LED_MODE=inject  log and send the LED commands (default: log)
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

static const unsigned char LED_ON_1[]  = {0x46,0x07,0x90,0x01,0x04,0x00,0x00,0x00,0x00};
static const unsigned char LED_ON_2[]  = {0x46,0x07,0x80,0x01,0x04,0x40,0x00,0x00,0x00};
static const unsigned char LED_OFF_1[] = {0x46,0x07,0x80,0x01,0x04,0x40,0x00,0x00,0x00};
static const unsigned char LED_OFF_2[] = {0x46,0x07,0x90,0x01,0x04,0x10,0x00,0x00,0x00};

static int (*real_submit)(struct libusb_transfer *);
static int (*real_bulk)(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned int);
static int (*real_release)(libusb_device_handle *, int);
static void (*real_close)(libusb_device_handle *);

static int mode_inject = -1;
static int led_is_on;

static void init(void)
{
    if (mode_inject >= 0)
        return;
    const char *m = getenv("ELAN_LED_MODE");
    mode_inject = m && strcmp(m, "inject") == 0;
    real_submit  = dlsym(RTLD_NEXT, "libusb_submit_transfer");
    real_bulk    = dlsym(RTLD_NEXT, "libusb_bulk_transfer");
    real_release = dlsym(RTLD_NEXT, "libusb_release_interface");
    real_close   = dlsym(RTLD_NEXT, "libusb_close");
    fprintf(stderr, "elan-led-shim: loaded, mode=%s\n", mode_inject ? "inject" : "log");
}

static int is_elan(libusb_device_handle *h)
{
    struct libusb_device_descriptor d;
    if (!h || libusb_get_device_descriptor(libusb_get_device(h), &d) != 0)
        return 0;
    return d.idVendor == ELAN_VID && d.idProduct == ELAN_PID;
}

static void log_cmd(const char *via, const unsigned char *b, int len)
{
    char hex[64] = "";
    for (int i = 0; i < len && i < 16; i++)
        snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", b[i]);
    fprintf(stderr, "elan-led-shim: %s OUT %s%s\n", via, hex, len > 16 ? "..." : "");
}

static void free_cb(struct libusb_transfer *t) { (void)t; }

/* Async send, used when the driver itself is async (may run inside the event loop). */
static void send_async(libusb_device_handle *h, const unsigned char *cmd, int len)
{
    struct libusb_transfer *t = libusb_alloc_transfer(0);
    unsigned char *buf = malloc(len);
    if (!t || !buf) { free(buf); if (t) libusb_free_transfer(t); return; }
    memcpy(buf, cmd, len);
    libusb_fill_bulk_transfer(t, h, EP_CMD, buf, len, free_cb, NULL, 1000);
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
    if (real_submit(t) != 0)
        libusb_free_transfer(t);
}

static void send_sync(libusb_device_handle *h, const unsigned char *cmd, int len)
{
    int done;
    real_bulk(h, EP_CMD, (unsigned char *)cmd, len, &done, 1000);
}

static void led(libusb_device_handle *h, int on, int async)
{
    if (!mode_inject || on == led_is_on)
        return;
    const unsigned char *a = on ? LED_ON_1 : LED_OFF_1, *b = on ? LED_ON_2 : LED_OFF_2;
    fprintf(stderr, "elan-led-shim: LED %s\n", on ? "green" : "restore");
    if (async) { send_async(h, a, 9); send_async(h, b, 9); }
    else       { send_sync(h, a, 9);  send_sync(h, b, 9); }
    led_is_on = on;
}

static int is_cmd(const unsigned char *b, int len, unsigned char c0, unsigned char c1)
{
    return len >= 2 && b[0] == c0 && b[1] == c1;
}

int libusb_submit_transfer(struct libusb_transfer *t)
{
    init();
    if (t->type != LIBUSB_TRANSFER_TYPE_BULK || t->endpoint != EP_CMD || !is_elan(t->dev_handle))
        return real_submit(t);
    log_cmd("async", t->buffer, t->length);
    if (is_cmd(t->buffer, t->length, 0x40, 0x3f))
        led(t->dev_handle, 1, 1);
    int r = real_submit(t);
    if (is_cmd(t->buffer, t->length, 0x00, 0x0b))
        led(t->dev_handle, 0, 1);
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
        led(h, 1, 0);
    int r = real_bulk(h, ep, data, len, transferred, timeout);
    if (is_cmd(data, len, 0x00, 0x0b))
        led(h, 0, 0);
    return r;
}

int libusb_release_interface(libusb_device_handle *h, int iface)
{
    init();
    if (is_elan(h))
        led(h, 0, 0);
    return real_release(h, iface);
}

void libusb_close(libusb_device_handle *h)
{
    init();
    if (is_elan(h))
        led(h, 0, 0);
    real_close(h);
}
