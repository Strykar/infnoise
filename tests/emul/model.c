/* Write the source model's raw bit stream to stdout.
 *
 * The stream is in raw capture format (the bits extractBytes() feeds to the
 * health check, packed MSB first), so it pipes straight into ./replay.
 * The model itself lives in source_model.h, shared with fake_libftdi.c.
 *
 * This is a test instrument.  It must never be used as an entropy source.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "source_model.h"

#define MODEL_BITS 10000000ull

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--bits N] [--seed S] [--k K] [--c C] [--u U] [--sigma S]\n"
        "          [--stuck 0|1]\n\n"
        "Writes a raw-format bit stream from the source model to stdout.\n"
        "Defaults are the fitted parameters.  Fault knobs: --k for gain\n"
        "drift, --u for comparator bias, --stuck forces the emitted\n"
        "comparator output to a constant while the loop keeps running.\n",
        argv0);
}

int main(int argc, char **argv) {
    uint64_t bits = MODEL_BITS;
    struct source_model m;
    uint64_t seed = MODEL_SEED;
    double k = MODEL_K, c = MODEL_C, u = MODEL_U, sigma = MODEL_SIGMA;
    int stuck = -1;
    uint64_t i;
    uint8_t byte = 0u;
    int nbits = 0;
    int j;

    for(j = 1; j < argc; j++) {
        if(!strcmp(argv[j], "--bits") && j + 1 < argc) {
            bits = strtoull(argv[++j], NULL, 0);
        } else if(!strcmp(argv[j], "--seed") && j + 1 < argc) {
            seed = strtoull(argv[++j], NULL, 0);
        } else if(!strcmp(argv[j], "--k") && j + 1 < argc) {
            k = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--c") && j + 1 < argc) {
            c = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--u") && j + 1 < argc) {
            u = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--sigma") && j + 1 < argc) {
            sigma = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--stuck") && j + 1 < argc) {
            stuck = atoi(argv[++j]) != 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    sourceModelInit(&m, seed);
    m.K = k;
    m.c = c;
    m.u = u;
    m.sigma = sigma;
    sourceModelWarmup(&m);

    for(i = 0u; i < bits; i++) {
        bool bit = sourceModelStep(&m);
        if(stuck >= 0) {
            bit = stuck;
        }
        byte = (uint8_t)((byte << 1) | bit);
        if(++nbits == 8) {
            if(putchar(byte) == EOF) {
                return 2;
            }
            byte = 0u;
            nbits = 0;
        }
    }
    return 0;
}
