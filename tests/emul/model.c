/* Seeded source model of the Infinite Noise multiplier.
 *
 * One analog value circulates through the two gain stages, one comparator
 * decision per half-cycle.  Each step follows the reference trick described
 * in upstream's README: the comparator decides against mid-scale first, and
 * that decision selects whether the gain stage multiplies relative to GND or
 * to the supply.  Normalised to [0,1):
 *
 *     bit = (A >= 1/2 + u)
 *     A'  = K*A - bit*(K-1) + (bit? -c : c) + sigma*gauss()
 *
 * For K = 2 and c = 0 this is exactly A' = 2A mod 1.  For K < 2 it differs
 * from the naive mod map: both branches land inside [0,1) on their own, and
 * the map commutes with x -> 1-x, so the ones fraction sits at one half by
 * symmetry instead of the mod map's ~42%.  c is the switch charge injection;
 * its sign follows the comparator because the two references are switched by
 * different devices, which keeps the symmetry while pushing the state off
 * both rails.  u is the comparator offset and trims the ones fraction.
 *
 * The two hardware stages are modelled with identical parameters, so the
 * alternation reduces to iterating one map.  Two independent loops were
 * tried and falsified: they produce no adjacent-bit anti-correlation, while
 * a single circulating value reproduces the measured lag profile and both
 * substream statistics.  See the README for the fit and the residual gaps.
 *
 * The emitted stream is in raw capture format, so it pipes into ./replay.
 * Parameters are fitted to the captures listed in the README and judged by
 * ./replay --fidelity; the captures are the authority, the mechanism above
 * is just why these knobs can reach the measured statistics.
 *
 * This is a test instrument.  It must never be used as an entropy source.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* Fitted 2026-07-26 against tests/samples/infnoise_raw.bin; see the README
 * for the sweep history and the acceptance bands. */
#define MODEL_K      1.84
#define MODEL_C      0.02
#define MODEL_U      0.0008
#define MODEL_SIGMA  0.0005
#define MODEL_SEED   0x1337c0dedbeefull
#define MODEL_BITS   10000000ull
#define MODEL_WARMUP 4096u

static uint64_t rngState;

static uint64_t nextRand(void) {
    uint64_t z = (rngState += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static double uniform01(void) {
    return (nextRand() >> 11) * 0x1.0p-53;
}

static double gauss(void) {
    static bool haveSpare = false;
    static double spare;
    double u, r, phi;

    if(haveSpare) {
        haveSpare = false;
        return spare;
    }
    u = uniform01();
    if(u < 1e-300) {
        u = 1e-300;
    }
    r = sqrt(-2.0 * log(u));
    phi = 2.0 * 3.14159265358979323846 * uniform01();
    spare = r * sin(phi);
    haveSpare = true;
    return r * cos(phi);
}

struct params {
    double K;
    double c;
    double u;
    double sigma;
};

static bool stepModel(double *A, const struct params *p) {
    bool bit = *A >= 0.5 + p->u;
    double x = p->K * *A - (bit? p->K - 1.0 : 0.0) + (bit? -p->c : p->c)
        + p->sigma * gauss();

    if(x < 0.0) {
        x = 0.0;
    } else if(x >= 1.0) {
        x = 1.0 - 1e-9;
    }
    *A = x;
    return bit;
}

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
    struct params p = { MODEL_K, MODEL_C, MODEL_U, MODEL_SIGMA };
    double A = 0.5;
    int stuck = -1;
    uint64_t i;
    uint8_t byte = 0u;
    int nbits = 0;
    int j;

    rngState = MODEL_SEED;

    for(j = 1; j < argc; j++) {
        if(!strcmp(argv[j], "--bits") && j + 1 < argc) {
            bits = strtoull(argv[++j], NULL, 0);
        } else if(!strcmp(argv[j], "--seed") && j + 1 < argc) {
            rngState = strtoull(argv[++j], NULL, 0);
        } else if(!strcmp(argv[j], "--k") && j + 1 < argc) {
            p.K = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--c") && j + 1 < argc) {
            p.c = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--u") && j + 1 < argc) {
            p.u = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--sigma") && j + 1 < argc) {
            p.sigma = atof(argv[++j]);
        } else if(!strcmp(argv[j], "--stuck") && j + 1 < argc) {
            stuck = atoi(argv[++j]) != 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    for(i = 0u; i < MODEL_WARMUP; i++) {
        (void)stepModel(&A, &p);
    }

    for(i = 0u; i < bits; i++) {
        bool bit = stepModel(&A, &p);
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
