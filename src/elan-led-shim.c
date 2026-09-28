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
 * Matching happens on the host, so a failed match looks the same as a good
 * one on USB. The library also wraps libfprint's fpi_device_verify_report()
 * and fpi_device_identify_report(); on "no match" it sends the Windows fail
 * flash (white flash, then green) and restores the LED 1.5 s later unless a
 * new scan has started.
 *
 * Environment:
 *   ELAN_LED_MODE=inject  send the LED commands (default)
 *   ELAN_LED_MODE=log     log every command and match result to stderr,
 *                         send nothing
 *   ELAN_LED_MODE=off     do nothing
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

#define ELAN_VID 0x04f3
#define ELAN_PID 0x0c4b
#define EP_CMD   0x01
#define CMD_LEN  9

#define FPRINT_TOD_VERSION "LIBFPRINT_TOD_1.0.0"
#define FAIL_RESTORE_DELAY_NS 1500000000L

/* FpiMatchResult from libfprint's fpi-device.h */
enum { FPI_MATCH_ERROR = -1, FPI_MATCH_FAIL = 0, FPI_MATCH_SUCCESS = 1 };

enum mode { MODE_UNSET = -1, MODE_OFF, MODE_LOG, MODE_INJECT };

static const unsigned char LED_GREEN[2][CMD_LEN] = {
    {0x46, 0x07, 0x90, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00},
    {0x46, 0x07, 0x80, 0x01, 0x04, 0x40, 0x00, 0x00, 0x00},
};
static const unsigned char LED_RESTORE[2][CMD_LEN] = {
    {0x46, 0x07, 0x80, 0x01, 0x04, 0x40, 0x00, 0x00, 0x00},
    {0x46, 0x07, 0x90, 0x01, 0x04, 0x10, 0x00, 0x00, 0x00},
};
static const unsigned char LED_FAIL[CMD_LEN] =
    {0x46, 0x07, 0x90, 0x01, 0x04, 0x20, 0xc0, 0x30, 0x00};

static int (*real_submit)(struct libusb_transfer *);
static int (*real_bulk)(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned int);
static int (*real_release)(libusb_device_handle *, int);
static void (*real_close)(libusb_device_handle *);
static void (*real_verify_report)(void *, int, void *, void *);
static void (*real_identify_report)(void *, void *, void *, void *);

static enum mode mode = MODE_UNSET;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/* Shared with the fail-flash timer thread, protected by lock. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static libusb_device_handle *elan_handle; /* last open ELAN handle, NULL once closed */
static int led_is_green;
static unsigned long scan_seq;            /* bumped on every 40 3f */

static void do_init(void)
{
    real_submit  = dlsym(RTLD_NEXT, "libusb_submit_transfer");
    real_bulk    = dlsym(RTLD_NEXT, "libusb_bulk_transfer");
    real_release = dlsym(RTLD_NEXT, "libusb_release_interface");
    real_close   = dlsym(RTLD_NEXT, "libusb_close");
    real_verify_report   = dlvsym(RTLD_NEXT, "fpi_device_verify_report", FPRINT_TOD_VERSION);
    real_identify_report = dlvsym(RTLD_NEXT, "fpi_device_identify_report", FPRINT_TOD_VERSION);
    if (!real_verify_report)
        real_verify_report = dlsym(RTLD_NEXT, "fpi_device_verify_report");
    if (!real_identify_report)
        real_identify_report = dlsym(RTLD_NEXT, "fpi_device_identify_report");

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

static void init(void)
{
    pthread_once(&init_once, do_init);
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

/* Caller holds lock. */
static void set_led_locked(libusb_device_handle *h, int green, int async)
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

/* Called before and after each ELAN command; before == 1 before submitting it. */
static void on_command(libusb_device_handle *h, const unsigned char *b, int len, int before, int async)
{
    pthread_mutex_lock(&lock);
    elan_handle = h;
    if (before && is_cmd(b, len, 0x40, 0x3f)) {
        scan_seq++;
        set_led_locked(h, 1, async);
    } else if (!before && is_cmd(b, len, 0x00, 0x0b)) {
        set_led_locked(h, 0, async);
    }
    pthread_mutex_unlock(&lock);
}

static void on_handle_gone(libusb_device_handle *h, const char *what)
{
    if (mode == MODE_LOG)
        fprintf(stderr, "elan-led-shim: %s\n", what);
    pthread_mutex_lock(&lock);
    set_led_locked(h, 0, 0);
    if (elan_handle == h)
        elan_handle = NULL;
    pthread_mutex_unlock(&lock);
}

static void *fail_restore_thread(void *arg)
{
    unsigned long seq = (unsigned long)arg;
    struct timespec ts = { FAIL_RESTORE_DELAY_NS / 1000000000L, FAIL_RESTORE_DELAY_NS % 1000000000L };
    nanosleep(&ts, NULL);

    pthread_mutex_lock(&lock);
    /* Skip if the device was closed or a new scan started meanwhile. */
    if (elan_handle && scan_seq == seq)
        set_led_locked(elan_handle, 0, 1);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void on_no_match(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    unsigned long seq;

    if (mode != MODE_INJECT)
        return;
    pthread_mutex_lock(&lock);
    if (!elan_handle) {
        pthread_mutex_unlock(&lock);
        return;
    }
    send_async(elan_handle, LED_FAIL);
    led_is_green = 1;       /* the fail flash ends in solid green */
    seq = scan_seq;
    pthread_mutex_unlock(&lock);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, fail_restore_thread, (void *)seq);
    pthread_attr_destroy(&attr);
}

int libusb_submit_transfer(struct libusb_transfer *t)
{
    init();
    if (t->type != LIBUSB_TRANSFER_TYPE_BULK || t->endpoint != EP_CMD || !is_elan(t->dev_handle))
        return real_submit(t);
    log_cmd("async", t->buffer, t->length);
    on_command(t->dev_handle, t->buffer, t->length, 1, 1);
    int r = real_submit(t);
    on_command(t->dev_handle, t->buffer, t->length, 0, 1);
    return r;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
                         int len, int *transferred, unsigned int timeout)
{
    init();
    if (ep != EP_CMD || !is_elan(h))
        return real_bulk(h, ep, data, len, transferred, timeout);
    log_cmd("sync", data, len);
    on_command(h, data, len, 1, 0);
    int r = real_bulk(h, ep, data, len, transferred, timeout);
    on_command(h, data, len, 0, 0);
    return r;
}

int libusb_release_interface(libusb_device_handle *h, int iface)
{
    init();
    if (is_elan(h))
        on_handle_gone(h, "release_interface");
    return real_release(h, iface);
}

void libusb_close(libusb_device_handle *h)
{
    init();
    if (is_elan(h))
        on_handle_gone(h, "close");
    real_close(h);
}

void fpi_device_verify_report(void *device, int result, void *print, void *error)
{
    init();
    if (mode == MODE_LOG) {
        fprintf(stderr, "elan-led-shim: verify result=%d%s\n", result, error ? " (with error)" : "");
    }
    if (mode != MODE_OFF && result == FPI_MATCH_FAIL && !error)
        on_no_match();
    if (real_verify_report)
        real_verify_report(device, result, print, error);
}

void fpi_device_identify_report(void *device, void *match, void *print, void *error)
{
    init();
    if (mode == MODE_LOG) {
        fprintf(stderr, "elan-led-shim: identify match=%s%s\n", match ? "yes" : "no",
                error ? " (with error)" : "");
    }
    if (mode != MODE_OFF && !match && !error)
        on_no_match();
    if (real_identify_report)
        real_identify_report(device, match, print, error);
}
