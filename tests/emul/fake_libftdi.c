/* Fake libftdi over the source model, so unmodified libinfnoise runs with no
 * hardware.
 *
 * Implements the 13 entry points libinfnoise calls.  It uses the real
 * <ftdi.h>: struct ftdi_context is embedded by value in struct
 * infnoise_context (libinfnoise.h:75), so the fake cannot pick its own
 * layout.  Only the functions are faked, never the types.
 *
 * Sample synthesis follows extractBytes().  For sample n the health check
 * reads COMP2 when n is odd and COMP1 when n is even, so the model advances
 * once per sample and the result latches into the comparator being read
 * while the other holds its level, which is what the hardware does between
 * clocks.  The driven output pins read back as written, since synchronous
 * bit-bang returns the state of every pin.  The one-byte read/write lag of
 * real sync bit-bang is NOT modelled; nothing libinfnoise reads depends on
 * it, and it remains unverified (workbook section 9).
 *
 * Faults are staged through the environment so tests need no recompile:
 *
 *   INFNOISE_FAKE_DEVICES   device count, default 1
 *   INFNOISE_FAKE_SERIALS   comma separated serials, default SN-FAKE-0001
 *   INFNOISE_FAKE_DECOYS    extra 0403:6015 devices with a non-INM product
 *                           string, to exercise product filtering
 *   INFNOISE_FAKE_SEED      model seed
 *   INFNOISE_FAKE_K         gain, for drift faults
 *   INFNOISE_FAKE_U         comparator offset, for bias faults
 *   INFNOISE_FAKE_STUCK     0 or 1, freeze the emitted comparator level
 *   INFNOISE_FAKE_FAULT     open_fail | baudrate_fail | bitmode_fail |
 *                           write_fail | short_read | read_fail | unplug
 *   INFNOISE_FAKE_FAULT_AT  number of successful reads before the fault
 *                           fires, default 0
 *
 * This is a test instrument.  It must never be used as an entropy source.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ftdi.h>

#include "source_model.h"

/* Pin assignment, from libinfnoise_private.h.  Duplicated rather than
 * included because that header pulls in the whole library's private API. */
#define FAKE_COMP1 1u
#define FAKE_COMP2 4u
#define FAKE_MASK  (0xffu & ~(1u << FAKE_COMP1) & ~(1u << FAKE_COMP2))

#define FAKE_MAX_DEVICES 16

enum fake_fault {
    FAULT_NONE = 0,
    FAULT_OPEN,
    FAULT_BAUDRATE,
    FAULT_BITMODE,
    FAULT_WRITE,
    FAULT_SHORT_READ,
    FAULT_READ,
    FAULT_UNPLUG
};

struct fake_device {
    char serial[64];
    char description[64];
};

static struct fake_device fakeDevices[FAKE_MAX_DEVICES];
static int fakeDeviceCount;
static bool fakeConfigured;

static enum fake_fault fakeFault;
static long fakeFaultAt;
static long fakeReadCount;

static bool fakeOpen;
static int fakeOpenIndex;
static const char *fakeError = "";

/* Model state, plus the two comparator levels and the sample parity. */
static struct source_model fakeModel;
static bool fakeComp[2];
static uint64_t fakeSampleIndex;
static int fakeStuck = -1;

/* Bytes clocked out but not yet read, exactly as the FT240X buffers them. */
#define FAKE_FIFO_MAX 65536
static unsigned char fakeFifo[FAKE_FIFO_MAX];
static size_t fakeFifoLen;

static enum fake_fault parseFault(const char *s) {
    if(s == NULL) {
        return FAULT_NONE;
    }
    if(!strcmp(s, "open_fail")) {
        return FAULT_OPEN;
    }
    if(!strcmp(s, "baudrate_fail")) {
        return FAULT_BAUDRATE;
    }
    if(!strcmp(s, "bitmode_fail")) {
        return FAULT_BITMODE;
    }
    if(!strcmp(s, "write_fail")) {
        return FAULT_WRITE;
    }
    if(!strcmp(s, "short_read")) {
        return FAULT_SHORT_READ;
    }
    if(!strcmp(s, "read_fail")) {
        return FAULT_READ;
    }
    if(!strcmp(s, "unplug")) {
        return FAULT_UNPLUG;
    }
    fprintf(stderr, "fake_libftdi: unknown INFNOISE_FAKE_FAULT '%s'\n", s);
    exit(2);
}

static void fakeConfigure(void) {
    const char *serials, *env;
    int nreal, ndecoy, i;

    if(fakeConfigured) {
        return;
    }
    fakeConfigured = true;

    env = getenv("INFNOISE_FAKE_DEVICES");
    nreal = env != NULL? atoi(env) : 1;
    env = getenv("INFNOISE_FAKE_DECOYS");
    ndecoy = env != NULL? atoi(env) : 0;
    if(nreal < 0) {
        nreal = 0;
    }
    if(nreal + ndecoy > FAKE_MAX_DEVICES) {
        ndecoy = FAKE_MAX_DEVICES - nreal;
    }

    serials = getenv("INFNOISE_FAKE_SERIALS");
    for(i = 0; i < nreal; i++) {
        const char *comma;
        size_t len;

        snprintf(fakeDevices[i].description, sizeof(fakeDevices[i].description),
            "Infinite Noise TRNG");
        if(serials == NULL) {
            snprintf(fakeDevices[i].serial, sizeof(fakeDevices[i].serial),
                "SN-FAKE-%04d", i + 1);
            continue;
        }
        comma = strchr(serials, ',');
        len = comma != NULL? (size_t)(comma - serials) : strlen(serials);
        if(len >= sizeof(fakeDevices[i].serial)) {
            len = sizeof(fakeDevices[i].serial) - 1u;
        }
        memcpy(fakeDevices[i].serial, serials, len);
        fakeDevices[i].serial[len] = '\0';
        if(comma != NULL) {
            serials = comma + 1;
        }
    }
    /* Decoys share 0403:6015, which every FT-X part does, and differ only by
     * product string.  Enumeration has to filter on that. */
    for(i = 0; i < ndecoy; i++) {
        snprintf(fakeDevices[nreal + i].description,
            sizeof(fakeDevices[nreal + i].description), "FT240X Basic UART");
        snprintf(fakeDevices[nreal + i].serial,
            sizeof(fakeDevices[nreal + i].serial), "SN-DECOY-%04d", i + 1);
    }
    fakeDeviceCount = nreal + ndecoy;

    fakeFault = parseFault(getenv("INFNOISE_FAKE_FAULT"));
    env = getenv("INFNOISE_FAKE_FAULT_AT");
    fakeFaultAt = env != NULL? atol(env) : 0;

    env = getenv("INFNOISE_FAKE_SEED");
    sourceModelInit(&fakeModel, env != NULL?
        strtoull(env, NULL, 0) : (uint64_t)MODEL_SEED);
    env = getenv("INFNOISE_FAKE_K");
    if(env != NULL) {
        fakeModel.K = atof(env);
    }
    env = getenv("INFNOISE_FAKE_U");
    if(env != NULL) {
        fakeModel.u = atof(env);
    }
    sourceModelWarmup(&fakeModel);

    env = getenv("INFNOISE_FAKE_STUCK");
    fakeStuck = env != NULL? (atoi(env) != 0) : -1;
}

/* Clock one byte through the loop and return the pin state read back. */
static unsigned char fakeClock(unsigned char driven) {
    bool bit = sourceModelStep(&fakeModel);
    unsigned int parity = (unsigned int)(fakeSampleIndex & 1u);

    if(fakeStuck >= 0) {
        bit = fakeStuck != 0;
    }
    /* extractBytes() takes COMP2 on odd samples and COMP1 on even ones; the
       comparator that is not being read holds its previous level. */
    fakeComp[parity] = bit;
    fakeSampleIndex++;

    return (unsigned char)((driven & FAKE_MASK)
        | ((unsigned int)fakeComp[0] << FAKE_COMP1)
        | ((unsigned int)fakeComp[1] << FAKE_COMP2));
}

int ftdi_init(struct ftdi_context *ftdi) {
    fakeConfigure();
    if(ftdi != NULL) {
        memset(ftdi, 0, sizeof(*ftdi));
    }
    fakeError = "";
    return 0;
}

void ftdi_deinit(struct ftdi_context *ftdi) {
    (void)ftdi;
}

const char *ftdi_get_error_string(struct ftdi_context *ftdi) {
    (void)ftdi;
    return fakeError;
}

int ftdi_usb_find_all(struct ftdi_context *ftdi, struct ftdi_device_list **devlist,
                      int vendor, int product) {
    struct ftdi_device_list *head = NULL, *tail = NULL;
    int i;

    (void)ftdi;
    fakeConfigure();
    *devlist = NULL;
    if(vendor != 0x0403 || product != 0x6015) {
        return 0;
    }
    for(i = 0; i < fakeDeviceCount; i++) {
        struct ftdi_device_list *node = calloc(1u, sizeof(*node));

        if(node == NULL) {
            fakeError = "out of memory";
            return -1;
        }
        /* The dev pointer is opaque to libinfnoise: it only ever hands it
           back to ftdi_usb_get_strings(), so index it by 1-based value. */
        node->dev = (struct libusb_device *)(uintptr_t)(i + 1);
        node->next = NULL;
        if(head == NULL) {
            head = node;
        } else {
            tail->next = node;
        }
        tail = node;
    }
    *devlist = head;
    return fakeDeviceCount;
}

void ftdi_list_free2(struct ftdi_device_list *devlist) {
    while(devlist != NULL) {
        struct ftdi_device_list *next = devlist->next;

        free(devlist);
        devlist = next;
    }
}

int ftdi_usb_get_strings(struct ftdi_context *ftdi, struct libusb_device *dev,
                         char *manufacturer, int mnf_len,
                         char *description, int desc_len,
                         char *serial, int serial_len) {
    int index = (int)(uintptr_t)dev - 1;

    (void)ftdi;
    fakeConfigure();
    if(index < 0 || index >= fakeDeviceCount) {
        fakeError = "no such device";
        return -1;
    }
    if(manufacturer != NULL && mnf_len > 0) {
        snprintf(manufacturer, (size_t)mnf_len, "13-37.org");
    }
    if(description != NULL && desc_len > 0) {
        snprintf(description, (size_t)desc_len, "%s",
            fakeDevices[index].description);
    }
    if(serial != NULL && serial_len > 0) {
        snprintf(serial, (size_t)serial_len, "%s", fakeDevices[index].serial);
    }
    return 0;
}

int ftdi_usb_open(struct ftdi_context *ftdi, int vendor, int product) {
    (void)ftdi;
    fakeConfigure();
    if(fakeFault == FAULT_OPEN) {
        fakeError = "device not found (staged)";
        return -3;
    }
    if(vendor != 0x0403 || product != 0x6015 || fakeDeviceCount == 0) {
        fakeError = "device not found";
        return -3;
    }
    fakeOpen = true;
    fakeOpenIndex = 0;
    return 0;
}

int ftdi_usb_open_desc(struct ftdi_context *ftdi, int vendor, int product,
                       const char *description, const char *serial) {
    int i;

    (void)ftdi;
    (void)description;
    fakeConfigure();
    if(fakeFault == FAULT_OPEN) {
        fakeError = "device not found (staged)";
        return -3;
    }
    if(vendor != 0x0403 || product != 0x6015) {
        fakeError = "device not found";
        return -3;
    }
    for(i = 0; i < fakeDeviceCount; i++) {
        if(serial == NULL || !strcmp(serial, fakeDevices[i].serial)) {
            fakeOpen = true;
            fakeOpenIndex = i;
            return 0;
        }
    }
    fakeError = "no device with that serial";
    return -3;
}

int ftdi_usb_close(struct ftdi_context *ftdi) {
    (void)ftdi;
    fakeOpen = false;
    return 0;
}

int ftdi_set_baudrate(struct ftdi_context *ftdi, int baudrate) {
    (void)ftdi;
    if(fakeFault == FAULT_BAUDRATE) {
        fakeError = "setting baud rate failed (staged)";
        return -2;
    }
    if(!fakeOpen) {
        fakeError = "device unavailable";
        return -3;
    }
    if(baudrate <= 0) {
        fakeError = "invalid baud rate";
        return -1;
    }
    return 0;
}

int ftdi_set_bitmode(struct ftdi_context *ftdi, unsigned char bitmask,
                     unsigned char mode) {
    (void)ftdi;
    if(fakeFault == FAULT_BITMODE) {
        fakeError = "can't enable bitbang mode (staged)";
        return -1;
    }
    if(!fakeOpen) {
        fakeError = "device unavailable";
        return -2;
    }
    if(mode != BITMODE_SYNCBB || bitmask != FAKE_MASK) {
        fakeError = "unexpected bit mode";
        return -1;
    }
    return 0;
}

int ftdi_write_data(struct ftdi_context *ftdi, const unsigned char *buf, int size) {
    int i;

    (void)ftdi;
    if(!fakeOpen
       || (fakeFault == FAULT_UNPLUG && fakeReadCount >= fakeFaultAt)) {
        fakeError = "device unavailable";
        return -1;
    }
    if(fakeFault == FAULT_WRITE) {
        fakeError = "usb bulk write failed (staged)";
        return -1;
    }
    /* Every clocked byte produces one sample, queued until it is read. */
    for(i = 0; i < size; i++) {
        if(fakeFifoLen >= sizeof(fakeFifo)) {
            break;
        }
        fakeFifo[fakeFifoLen++] = fakeClock(buf[i]);
    }
    return size;
}

int ftdi_read_data(struct ftdi_context *ftdi, unsigned char *buf, int size) {
    int avail;

    (void)ftdi;
    if(!fakeOpen) {
        fakeError = "device unavailable";
        return -1;
    }
    if(fakeReadCount >= fakeFaultAt) {
        if(fakeFault == FAULT_UNPLUG) {
            fakeError = "device disappeared (staged)";
            return -1;
        }
        if(fakeFault == FAULT_READ) {
            fakeError = "usb bulk read failed (staged)";
            return -1;
        }
    }
    avail = (int)fakeFifoLen < size? (int)fakeFifoLen : size;
    if(fakeFault == FAULT_SHORT_READ && fakeReadCount >= fakeFaultAt
       && avail > 1) {
        avail /= 2;
    }
    memcpy(buf, fakeFifo, (size_t)avail);
    memmove(fakeFifo, fakeFifo + avail, fakeFifoLen - (size_t)avail);
    fakeFifoLen -= (size_t)avail;
    fakeReadCount++;
    return avail;
}
