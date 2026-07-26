/* Seeded source model of the Infinite Noise multiplier.
 *
 * Header-only so both model.c (raw stream to stdout) and fake_libftdi.c
 * (synthetic FT240X samples) share one copy of the fitted parameters.
 *
 * One analog value circulates through the two gain stages, one comparator
 * decision per half-cycle, following the reference trick in upstream's
 * README: the comparator decides against mid-scale first, and that decision
 * selects whether the gain stage multiplies relative to GND or to the
 * supply.  Normalised to [0,1):
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
 * substream statistics.  See README.md for the fit and the residual gaps.
 *
 * This is a test instrument.  It must never be used as an entropy source.
 */
#ifndef SOURCE_MODEL_H
#define SOURCE_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* Fitted 2026-07-26 against tests/samples/infnoise_raw.bin; see README.md
 * for the sweep history and the acceptance bands. */
#define MODEL_K      1.84
#define MODEL_C      0.02
#define MODEL_U      0.0008
#define MODEL_SIGMA  0.0005
#define MODEL_SEED   0x1337c0dedbeefull
#define MODEL_WARMUP 4096u

struct source_model {
    double K;
    double c;
    double u;
    double sigma;
    double A;
    uint64_t rng;
    bool haveSpare;
    double spare;
};

static inline uint64_t modelNextRand(struct source_model *m) {
    uint64_t z = (m->rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static inline double modelUniform01(struct source_model *m) {
    return (modelNextRand(m) >> 11) * 0x1.0p-53;
}

static inline double modelGauss(struct source_model *m) {
    double u, r, phi;

    if(m->haveSpare) {
        m->haveSpare = false;
        return m->spare;
    }
    u = modelUniform01(m);
    if(u < 1e-300) {
        u = 1e-300;
    }
    r = sqrt(-2.0 * log(u));
    phi = 2.0 * 3.14159265358979323846 * modelUniform01(m);
    m->spare = r * sin(phi);
    m->haveSpare = true;
    return r * cos(phi);
}

/* One comparator decision, one half-cycle of the loop. */
static inline bool sourceModelStep(struct source_model *m) {
    bool bit = m->A >= 0.5 + m->u;
    double x = m->K * m->A - (bit? m->K - 1.0 : 0.0) + (bit? -m->c : m->c)
        + m->sigma * modelGauss(m);

    if(x < 0.0) {
        x = 0.0;
    } else if(x >= 1.0) {
        x = 1.0 - 1e-9;
    }
    m->A = x;
    return bit;
}

/* Set the fitted defaults.  Any parameter override must happen between this
 * and sourceModelWarmup(), so the warmup settles the map the caller will
 * actually run; warming up with different parameters shifts the statistics. */
static inline void sourceModelInit(struct source_model *m, uint64_t seed) {
    m->K = MODEL_K;
    m->c = MODEL_C;
    m->u = MODEL_U;
    m->sigma = MODEL_SIGMA;
    m->A = 0.5;
    m->rng = seed;
    m->haveSpare = false;
    m->spare = 0.0;
}

static inline void sourceModelWarmup(struct source_model *m) {
    uint32_t i;

    for(i = 0u; i < MODEL_WARMUP; i++) {
        (void)sourceModelStep(m);
    }
}

#endif
