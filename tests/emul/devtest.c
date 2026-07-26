/* Drive unmodified libinfnoise against the fake libftdi.
 *
 * Everything below the API boundary is upstream's real code: enumeration,
 * initializeUSB, the sampling protocol, extractBytes, the health check and
 * Keccak.  Only libftdi is faked, so this exercises the library the way the
 * CLI does, with no hardware.
 *
 * Modes:
 *   list                   enumerate and print each device
 *   raw <bytes>            read raw mode, write the stream to stdout
 *   run <bytes>            read whitened mode, discard, report success
 *   open [serial]          initialise only, report the message on failure
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "libinfnoise.h"

static int doList(void) {
    const char *message = NULL;
    infnoise_devlist_node_t *list = listUSBDevices(&message), *node;
    int n = 0;

    if(list == NULL) {
        fprintf(stderr, "list: %s\n", message != NULL? message : "no devices");
        return 1;
    }
    for(node = list; node != NULL; node = node->next) {
        printf("device %d: manufacturer=%s description=%s serial=%s\n",
            n++, node->manufacturer, node->description, node->serial);
    }
    return 0;
}

/* A source whose entropy never lands on target makes readData return the
 * transient 0 forever, which is correct library behaviour but would hang a
 * caller that retries unconditionally.  Give up after this many consecutive
 * empty reads; the healthy path needs a few hundred to fill the 80000-bit
 * health window, so the cap is well clear of normal startup. */
#define MAX_EMPTY_READS 4000

static int doRead(const char *mode, uint64_t wanted, char *serial) {
    struct infnoise_context context;
    bool raw = !strcmp(mode, "raw");
    uint64_t written = 0u;
    long empty = 0;

    if(!initInfnoise(&context, serial, !raw, false)) {
        fprintf(stderr, "init: %s\n", context.message);
        return 1;
    }
    while(written < wanted) {
        uint8_t result[1024];
        int32_t got = readData(&context, result, raw, 1u);

        if(got < 0) {
            fprintf(stderr, "read: %s (code %d)\n", context.message, (int)got);
            deinitInfnoise(&context);
            return 1;
        }
        if(got == 0) {
            /* Transient: timing exceeded or entropy off target, so retry. */
            if(++empty >= MAX_EMPTY_READS) {
                fprintf(stderr, "read: no on-target entropy after %d reads\n",
                    MAX_EMPTY_READS);
                deinitInfnoise(&context);
                return 1;
            }
            continue;
        }
        empty = 0;
        if(raw && fwrite(result, 1u, (size_t)got, stdout) != (size_t)got) {
            deinitInfnoise(&context);
            return 1;
        }
        written += (uint64_t)got;
    }
    deinitInfnoise(&context);
    return 0;
}

static int doOpen(char *serial) {
    struct infnoise_context context;

    if(!initInfnoise(&context, serial, true, false)) {
        fprintf(stderr, "init: %s\n", context.message);
        return 1;
    }
    fprintf(stderr, "init: ok\n");
    deinitInfnoise(&context);
    return 0;
}

int main(int argc, char **argv) {
    if(argc < 2) {
        fprintf(stderr, "usage: %s list | raw <bytes> | run <bytes> | open [serial]\n",
            argv[0]);
        return 2;
    }
    if(!strcmp(argv[1], "list")) {
        return doList();
    }
    if(!strcmp(argv[1], "open")) {
        return doOpen(argc > 2? argv[2] : NULL);
    }
    if((!strcmp(argv[1], "raw") || !strcmp(argv[1], "run")) && argc > 2) {
        return doRead(argv[1], strtoull(argv[2], NULL, 0),
            argc > 3? argv[3] : NULL);
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
