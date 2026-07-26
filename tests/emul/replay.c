/* Replay a raw INM capture through the health check and check the verdict.
 *
 * Raw mode output is exactly the bit stream extractBytes() feeds to the health
 * check: byte = (byte << 1) | bit, so bit j of byte i (j=0 is the MSB) carries
 * even = j & 1.  Only the selected comparator affects the entropy estimate and
 * the run check.  The unselected one feeds evenMisfires/oddMisfires, which are
 * debug counters that inmHealthCheckOkToUseData() never reads, so driving both
 * arguments with the same bit reproduces the real verdict.
 *
 * The uniform mode is the negative control.  Uniform random measures about 1.0
 * bits per bit against a target of log2(1.84) = 0.88, so it fails the upper
 * bound of the accuracy band, and it also produces runs past INM_MAX_SEQUENCE.
 * A fake source that emits uniform bits is rejected by the device's own health
 * check, which is the property this harness exists to pin down.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "libinfnoise_private.h"

#define UNIFORM_SEED 0x0123456789abcdefull

struct replay_stats {
    uint64_t bits;
    uint64_t ones;
    uint64_t runFailures;
    uint32_t maxRun;
    double entropyPerBit;
    double estimatedK;
    bool okToUseData;
};

/* splitmix64, so the negative control is identical on every host */
static uint64_t nextUniform(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void addByte(struct infnoise_health_state *hc, uint8_t byte,
                    struct replay_stats *st, bool *haveLast, bool *lastBit,
                    uint32_t *runLen) {
    uint32_t j;
    for(j = 0u; j < 8u; j++) {
        bool bit = (byte >> (7u - j)) & 1u;
        bool even = j & 1u;
        if(bit) {
            st->ones++;
        }
        if(*haveLast && bit == *lastBit) {
            (*runLen)++;
        } else {
            *runLen = 1u;
        }
        *lastBit = bit;
        *haveLast = true;
        if(*runLen > st->maxRun) {
            st->maxRun = *runLen;
        }
        /* Keep going after a failure so the whole stream is measured.  A
           failed AddBit only resets its run counter, it does not corrupt the
           predictor state. */
        if(!inmHealthCheckAddBit(hc, bit, bit, even)) {
            st->runFailures++;
        }
        st->bits++;
    }
}

static bool replay(FILE *in, uint64_t uniformBytes, struct replay_stats *st) {
    struct infnoise_health_state hc;
    bool haveLast = false, lastBit = false;
    uint32_t runLen = 0u;
    uint64_t uniformState = UNIFORM_SEED;

    /* inmHealthCheckStart() does not initialise prevEven/prevOdd */
    memset(&hc, 0, sizeof(hc));
    memset(st, 0, sizeof(*st));
    if(!inmHealthCheckStart(&hc, PREDICTION_BITS, DESIGN_K, false)) {
        fprintf(stderr, "replay: inmHealthCheckStart failed\n");
        return false;
    }

    if(in != NULL) {
        int c;
        while((c = fgetc(in)) != EOF) {
            addByte(&hc, (uint8_t)c, st, &haveLast, &lastBit, &runLen);
        }
    } else {
        uint64_t i;
        for(i = 0u; i < uniformBytes; i++) {
            addByte(&hc, (uint8_t)nextUniform(&uniformState), st,
                    &haveLast, &lastBit, &runLen);
        }
    }

    st->entropyPerBit = inmHealthCheckEstimateEntropyPerBit(&hc);
    st->estimatedK = inmHealthCheckEstimateK(&hc);
    st->okToUseData = inmHealthCheckOkToUseData(&hc);
    inmHealthCheckStop(&hc);
    return true;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--expect pass|fail] <file>\n"
        "       %s [--expect pass|fail] --uniform <bytes>\n\n"
        "Replays a raw capture through the health check and reports the verdict.\n"
        "--uniform replays seeded uniform random data instead, as a negative control.\n"
        "With --expect, exits non-zero when the verdict differs.\n", argv0, argv0);
}

int main(int argc, char **argv) {
    const char *path = NULL, *expect = NULL;
    uint64_t uniformBytes = 0u;
    struct replay_stats st;
    double target, lower, upper;
    int i;

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--expect") && i + 1 < argc) {
            expect = argv[++i];
            if(strcmp(expect, "pass") && strcmp(expect, "fail")) {
                usage(argv[0]);
                return 2;
            }
        } else if(!strcmp(argv[i], "--uniform") && i + 1 < argc) {
            uniformBytes = strtoull(argv[++i], NULL, 0);
        } else if(argv[i][0] != '-') {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if((path == NULL) == (uniformBytes == 0u)) {
        usage(argv[0]);
        return 2;
    }

    if(path != NULL) {
        FILE *in = fopen(path, "rb");
        if(in == NULL) {
            fprintf(stderr, "replay: cannot open %s\n", path);
            return 2;
        }
        if(!replay(in, 0u, &st)) {
            fclose(in);
            return 2;
        }
        fclose(in);
    } else if(!replay(NULL, uniformBytes, &st)) {
        return 2;
    }

    if(st.bits == 0u) {
        fprintf(stderr, "replay: no data\n");
        return 2;
    }

    target = log(DESIGN_K)/log(2.0);
    lower = target/INM_ACCURACY;
    upper = target*INM_ACCURACY;

    printf("%s\n", path != NULL ? path : "uniform random (seeded)");
    printf("  bits             %llu\n", (unsigned long long)st.bits);
    printf("  ones             %.3f%%\n", st.ones*100.0/st.bits);
    printf("  entropy per bit  %.4f  (target %.4f, band %.4f to %.4f)\n",
        st.entropyPerBit, target, lower, upper);
    printf("  estimated K      %.4f  (DESIGN_K %.2f)\n", st.estimatedK, DESIGN_K);
    printf("  longest run      %u\n", st.maxRun);
    printf("  run failures     %llu\n", (unsigned long long)st.runFailures);
    printf("  verdict          %s\n", st.okToUseData? "PASS" : "FAIL");

    if(expect != NULL) {
        bool want = !strcmp(expect, "pass");
        /* A capture that trips the run check is not usable even if the
           entropy estimate lands in the band. */
        bool got = st.okToUseData && st.runFailures == 0u;
        if(got != want) {
            printf("  EXPECTED         %s\n", want? "PASS" : "FAIL");
            return 1;
        }
    }
    return 0;
}
