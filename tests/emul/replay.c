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

/* Fidelity bands, measured from the three device captures listed in the
 * README.  These are properties of the recorded hardware output, not of the
 * health check: a source can sit inside the entropy band and still look
 * nothing like the device.  The run cliff is the tightest invariant found:
 * runs of 1 to 3 decay at about 0.52 per step, then N(4)/N(3) collapses to
 * about 0.007. */
#define FIDELITY_MIN_BITS   5000000ull
#define FIDELITY_ONES_LO    0.500
#define FIDELITY_ONES_HI    0.520
#define FIDELITY_LAG1_LO    (-0.28)
#define FIDELITY_LAG1_HI    (-0.20)
#define FIDELITY_CLIFF_LO   0.003
#define FIDELITY_CLIFF_HI   0.015
#define FIDELITY_MAX_RUN    8u

#define RUN_HIST_MAX 32u

struct replay_stats {
    uint64_t bits;
    uint64_t ones;
    uint64_t runFailures;
    uint32_t maxRun;
    uint64_t runHist[RUN_HIST_MAX + 1u];
    uint64_t pairs;
    uint64_t pair11;
    bool haveLast;
    bool lastBit;
    uint32_t runLen;
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

static void recordRun(struct replay_stats *st) {
    uint32_t n = st->runLen > RUN_HIST_MAX? RUN_HIST_MAX : st->runLen;
    st->runHist[n]++;
}

static void addByte(struct infnoise_health_state *hc, uint8_t byte,
                    struct replay_stats *st) {
    uint32_t j;
    for(j = 0u; j < 8u; j++) {
        bool bit = (byte >> (7u - j)) & 1u;
        bool even = j & 1u;
        if(bit) {
            st->ones++;
        }
        if(st->haveLast) {
            st->pairs++;
            if(bit && st->lastBit) {
                st->pair11++;
            }
        }
        if(st->haveLast && bit == st->lastBit) {
            st->runLen++;
        } else {
            if(st->haveLast) {
                recordRun(st);
            }
            st->runLen = 1u;
        }
        st->lastBit = bit;
        st->haveLast = true;
        if(st->runLen > st->maxRun) {
            st->maxRun = st->runLen;
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
            addByte(&hc, (uint8_t)c, st);
        }
    } else {
        uint64_t i;
        for(i = 0u; i < uniformBytes; i++) {
            addByte(&hc, (uint8_t)nextUniform(&uniformState), st);
        }
    }
    if(st->haveLast) {
        recordRun(st);
    }

    st->entropyPerBit = inmHealthCheckEstimateEntropyPerBit(&hc);
    st->estimatedK = inmHealthCheckEstimateK(&hc);
    st->okToUseData = inmHealthCheckOkToUseData(&hc);
    inmHealthCheckStop(&hc);
    return true;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--expect pass|fail] [--fidelity] <file>\n"
        "       %s [--expect pass|fail] --uniform <bytes>\n\n"
        "Replays a raw capture through the health check and reports the verdict.\n"
        "<file> may be - for stdin.\n"
        "--uniform replays seeded uniform random data instead, as a negative control.\n"
        "--fidelity additionally checks the capture-derived bands: ones fraction,\n"
        "lag-1 autocorrelation, run cliff N(4)/N(3), and longest run.\n"
        "With --expect, exits non-zero when the verdict differs.\n", argv0, argv0);
}

int main(int argc, char **argv) {
    const char *path = NULL, *expect = NULL;
    uint64_t uniformBytes = 0u;
    struct replay_stats st;
    double target, lower, upper, m, lag1, cliff;
    bool fidelity = false;
    int fidelityBad = 0;
    uint32_t r;
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
        } else if(!strcmp(argv[i], "--fidelity")) {
            fidelity = true;
        } else if(!strcmp(argv[i], "-") || argv[i][0] != '-') {
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
        FILE *in = !strcmp(path, "-")? stdin : fopen(path, "rb");
        if(in == NULL) {
            fprintf(stderr, "replay: cannot open %s\n", path);
            return 2;
        }
        if(!replay(in, 0u, &st)) {
            if(in != stdin) {
                fclose(in);
            }
            return 2;
        }
        if(in != stdin) {
            fclose(in);
        }
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
    m = (double)st.ones/st.bits;
    lag1 = ((double)st.pair11/st.pairs - m*m)/(m*(1.0 - m));
    cliff = st.runHist[3] == 0u? -1.0 : (double)st.runHist[4]/st.runHist[3];

    printf("%s\n", path != NULL ? path : "uniform random (seeded)");
    printf("  bits             %llu\n", (unsigned long long)st.bits);
    printf("  ones             %.3f%%\n", m*100.0);
    printf("  entropy per bit  %.4f  (target %.4f, band %.4f to %.4f)\n",
        st.entropyPerBit, target, lower, upper);
    printf("  estimated K      %.4f  (DESIGN_K %.2f)\n", st.estimatedK, DESIGN_K);
    printf("  longest run      %u\n", st.maxRun);
    printf("  lag1 autocorr    %+.4f\n", lag1);
    printf("  run cliff        %.4f  (N4/N3)\n", cliff);
    printf("  runs 1..8       ");
    for(r = 1u; r <= 8u; r++) {
        printf(" %llu", (unsigned long long)st.runHist[r]);
    }
    printf("\n");
    printf("  run failures     %llu\n", (unsigned long long)st.runFailures);
    printf("  verdict          %s\n", st.okToUseData? "PASS" : "FAIL");

    if(fidelity) {
        if(st.bits < FIDELITY_MIN_BITS) {
            printf("  fidelity         FAIL (need %llu bits, got %llu)\n",
                (unsigned long long)FIDELITY_MIN_BITS, (unsigned long long)st.bits);
            fidelityBad++;
        } else {
            if(m < FIDELITY_ONES_LO || m > FIDELITY_ONES_HI) {
                fidelityBad++;
            }
            if(lag1 < FIDELITY_LAG1_LO || lag1 > FIDELITY_LAG1_HI) {
                fidelityBad++;
            }
            if(cliff < FIDELITY_CLIFF_LO || cliff > FIDELITY_CLIFF_HI) {
                fidelityBad++;
            }
            if(st.maxRun > FIDELITY_MAX_RUN) {
                fidelityBad++;
            }
            printf("  fidelity         %s (ones %.3f in [%.3f,%.3f], lag1 %+.3f in"
                " [%.2f,%.2f], cliff %.4f in [%.3f,%.3f], run %u <= %u)\n",
                fidelityBad == 0? "PASS" : "FAIL",
                m, FIDELITY_ONES_LO, FIDELITY_ONES_HI,
                lag1, FIDELITY_LAG1_LO, FIDELITY_LAG1_HI,
                cliff, FIDELITY_CLIFF_LO, FIDELITY_CLIFF_HI,
                st.maxRun, FIDELITY_MAX_RUN);
        }
    }

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
    return fidelityBad == 0? 0 : 1;
}
