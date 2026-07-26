# Health check replay, source model, and fake libftdi

Three tools, each building on the last, all running with no hardware and no
privileges:

- `replay` runs a raw capture through `software/healthcheck.c` and checks the
  verdict, plus fidelity bands measured from real devices.
- `model` is a seeded source model of the multiplier, judged by `replay`.
- `fake_libftdi.c` puts that model behind the 13 libftdi entry points, so
  **unmodified libinfnoise** runs against it. `devtest` drives the real API.

```
make check          # everything
make check-replay   # stage 0, captures only
make check-model    # stage 1, model only
make check-device   # stage 2, real libinfnoise over the fake
```

## Why uniform random has to fail

The health check requires measured entropy within `INM_ACCURACY` of
`log2(DESIGN_K)`, a two-sided band: 0.854 to 0.906 bits per bit. It also
rejects runs longer than `INM_MAX_SEQUENCE`. Uniform random measures about
1.0 bits per bit and runs past 20, so it fails both ways. Anything that fakes
this device by emitting uniform bits is rejected by the device's own health
check. The whitened capture is Keccak output, measures like uniform random,
and correctly fails: whitened data is not meant to re-enter the health check.

## Fidelity bands

`replay --fidelity` checks bands measured from two devices (upstream's
`tests/samples/infnoise_raw.bin` and a locally captured stick):

| statistic | band | device values |
|---|---|---|
| ones fraction | 0.500 to 0.520 | 0.5054, 0.5118 |
| lag-1 autocorrelation | -0.28 to -0.20 | -0.241, -0.245 |
| run cliff N(4)/N(3) | 0.003 to 0.015 | 0.0065, 0.0078 |
| longest run | at most 8 | 6, 7 |

These are properties of the recorded hardware, not of the health check: a
source can sit inside the entropy band and still look nothing like the
device. The run cliff is the tightest invariant: runs of 1 to 3 decay at
about 0.52 per step, then N(4)/N(3) collapses by two orders of magnitude.

## The model

One analog value circulates through the two gain stages, one comparator
decision per half-cycle, following the reference trick in upstream's README:

    bit = (A >= 1/2 + u);  A' = K*A - bit*(K-1) + (bit? -c : c) + sigma*gauss()

Fitted: K = 1.84, c = 0.02, u = 0.0008, sigma = 0.0005, seeded, so every run
reproduces. It measures (10M bits) ones 50.16%, entropy 0.8814, estimated K
1.8422, lag1 -0.234, cliff 0.0117, longest run 4, and its substream P(repeat)
0.436 sits between the device's 0.432 and 0.440.

Two findings from the fit, recorded so nobody refits them the hard way:

- The naive beta map `A' = K*A mod 1` is the wrong map for K < 2. It puts the
  comparator after the multiply, yields about 42% ones, and its runs trip the
  health check. The circuit's own map (comparator at mid-scale selecting the
  reference) has reflection symmetry, which is where the measured ~50% ones
  and the anti-correlation actually come from.
- Two independent loops were tried and falsified: interleaving them gives no
  adjacent-bit anti-correlation (device: -0.24) however they are coupled. A
  single circulating value produces the whole measured lag profile.

Known gaps, disclosed rather than fitted away:

- The model's longest run is a hard 4; the device tails softly to 6 or 7.
  Something slow (drift, supply wander) occasionally relaxes the bound on
  real hardware and is not modelled.
- Model ones sit at 50.16%, below the two devices' 50.5 and 51.2, though
  inside the band.
- `--sigma 0` still passes the health check: a deterministic chaotic map is
  indistinguishable from the noisy one at this predictor depth. The health
  check cannot certify physical randomness, which is one more reason this
  model must never be used as an entropy source.

## The fake libftdi

It uses the real `<ftdi.h>`. `struct ftdi_context` is embedded **by value** in
`struct infnoise_context` (`libinfnoise.h:75`), so the fake cannot choose its
own layout: only the functions are faked, never the types.

Sample synthesis follows `extractBytes()`. For sample n the health check
reads COMP2 when n is odd and COMP1 when n is even, so the model advances
once per clocked byte and the result latches into the comparator being read
while the other holds its level, as the hardware does between clocks. Driven
output pins read back as written, since synchronous bit-bang returns every
pin's state. The one-byte read/write lag of real sync bit-bang is **not**
modelled; nothing libinfnoise reads depends on it, and it is still unverified.

Verified end to end: `devtest raw` output matches the model's stream
byte-for-byte. Emission begins at model sample 177216, which is the 64-sample
probe plus 346 discarded warmup blocks, and the only discontinuities are
whole 512-sample gaps where `readData` returned its transient 0 because the
entropy gate rejected that block. Those splices are why the read path shows a
longest run of 7 while the model alone walls at 4: a splice joins two
non-adjacent segments. That is real library behaviour, not a fake artifact.

Faults are staged through the environment, so tests need no recompile:

| variable | effect |
|---|---|
| `INFNOISE_FAKE_DEVICES` | device count, default 1 |
| `INFNOISE_FAKE_SERIALS` | comma separated serials |
| `INFNOISE_FAKE_DECOYS` | extra 0403:6015 devices with a non-INM product string |
| `INFNOISE_FAKE_SEED` | model seed |
| `INFNOISE_FAKE_K` | gain, for drift faults |
| `INFNOISE_FAKE_U` | comparator offset, for bias faults |
| `INFNOISE_FAKE_STUCK` | 0 or 1, freeze the emitted comparator level |
| `INFNOISE_FAKE_FAULT` | `open_fail`, `baudrate_fail`, `bitmode_fail`, `write_fail`, `short_read`, `read_fail`, `unplug` |
| `INFNOISE_FAKE_FAULT_AT` | successful reads before the fault fires |

`faults.sh` runs the matrix. Each case asserts both that the call failed and
that libinfnoise reported the specific documented diagnostic, so a fault
cannot pass by failing for the wrong reason.

Decoys share 0403:6015, which every FT-X part does, and differ only by
product string. That is what enumeration has to filter on.

## Upstream defect found

`initInfnoise()` (`software/libinfnoise.c:64-75`) can loop forever:

```c
uint32_t maxWarmupRounds = 5000;
uint32_t warmupRounds = 0;

while (!inmHealthCheckOkToUseData(&context->health)) {
    readData(context, NULL, true, 1);
    warmupRounds++;
}

if (warmupRounds > maxWarmupRounds) { ... return false; }
```

The cap is declared but never tested inside the loop, and `readData`'s return
value is discarded. Two consequences:

- A source that never reaches on-target entropy hangs the caller forever
  instead of returning an error. A stuck comparator, a badly biased one, and
  a device unplugged during warmup all reproduce this in seconds.
- A device that is merely slow, needing more than 5000 rounds but working,
  gets a spurious failure, because the check runs only after the loop that it
  was meant to bound.

A healthy device needs about 346 rounds, so the second case is not reachable
in practice today; the first is. `faults.sh` pins the current behaviour with
`expect_hang`. When upstream bounds the loop, those three cases flip to
`expect_fail` and the diagnostic can be asserted instead.

## Adding a capture

Record with `infnoise --raw`, then:

    ./replay --expect pass --fidelity yourfile.bin

Raw mode writes the bit stream the health check saw, packed MSB first, so no
conversion is needed.
