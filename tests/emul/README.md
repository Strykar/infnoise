# Health check replay and source model

Two tools. `replay` runs a raw capture through `software/healthcheck.c` and
checks the verdict. `model` is a seeded source model of the multiplier whose
output is judged by `replay` against bands measured from real captures.

    make check

runs eight cases, no hardware and no privileges:

| input | expected | via |
|---|---|---|
| `infnoise_raw.bin` | PASS + fidelity | real capture |
| seeded uniform random | FAIL | entropy band and run check |
| `infnoise_whitened.bin` | FAIL | entropy band and run check |
| `model` (fitted defaults) | PASS + fidelity | |
| `model --k 1.8952` | FAIL | gain +3%, entropy above band |
| `model --k 1.7848` | FAIL | gain -3%, entropy below band |
| `model --u 0.05` | FAIL | comparator bias, entropy below band |
| `model --stuck 0` | FAIL | run check |

## Why uniform random has to fail

The health check requires measured entropy within `INM_ACCURACY` of
`log2(DESIGN_K)`, a two-sided band: 0.854 to 0.906 bits per bit. It also
rejects runs longer than `INM_MAX_SEQUENCE`. Uniform random measures about
1.0 bits per bit and runs past 20, so it fails both ways. Anything that fakes
this device by emitting uniform bits is rejected by the device's own health
check. The whitened capture is Keccak output, measures like uniform random,
and correctly fails: whitened data is not meant to re-enter the health check.

## Fidelity bands

`replay --fidelity` additionally checks bands measured from two devices
(upstream's `tests/samples/infnoise_raw.bin` and a locally captured stick):

| statistic | band | device values |
|---|---|---|
| ones fraction | 0.500 to 0.520 | 0.5054, 0.5118 |
| lag-1 autocorrelation | -0.28 to -0.20 | -0.241, -0.245 |
| run cliff N(4)/N(3) | 0.003 to 0.015 | 0.0065, 0.0078 |
| longest run | at most 8 | 6, 7 |

These are properties of the recorded hardware output, not of the health
check: a source can sit inside the entropy band and still look nothing like
the device. The run cliff is the tightest invariant: runs of 1 to 3 decay at
about 0.52 per step, then N(4)/N(3) collapses by two orders of magnitude.

## The model

One analog value circulates through the two gain stages, one comparator
decision per half-cycle, following the reference trick in upstream's README:

    bit = (A >= 1/2 + u);  A' = K*A - bit*(K-1) + (bit? -c : c) + sigma*gauss()

Fitted parameters: K = 1.84, c = 0.02, u = 0.0008, sigma = 0.0005, seeded, so
every run reproduces. With them the model measures (10M bits): ones 50.16%,
entropy 0.8814, estimated K 1.8422, lag1 -0.234, cliff 0.0117, longest run 4,
and passes health check plus fidelity. Substream statistics land on the
device's: P(repeat) 0.436 per comparator stream against 0.432 and 0.440
measured, and lags 2 to 4 match to about 0.005.

Two findings from the fit, recorded so nobody refits them the hard way:

- The naive beta map `A' = K*A mod 1` is the wrong map for K < 2. It puts the
  comparator after the multiply, yields about 42% ones, and its runs trip the
  health check. The circuit's own map (comparator at mid-scale selecting the
  reference) has reflection symmetry, which is where the measured ~50% ones
  and the anti-correlation actually come from.
- Two independent loops were tried and falsified: interleaving them gives no
  adjacent-bit anti-correlation (device: -0.24) however they are coupled.
  A single circulating value produces the whole measured lag profile.

Known gaps, disclosed rather than fitted away:

- The model's longest run is a hard 4; the device tails softly to 6 or 7.
  Something slow (drift, supply wander) occasionally relaxes the bound on
  real hardware and is not modelled.
- Model ones sit at 50.16%, below the two devices' 50.5 and 51.2, though
  inside the fidelity band.
- `--sigma 0` still passes the health check: a deterministic chaotic map is
  indistinguishable from the noisy one at this predictor depth. The health
  check cannot certify physical randomness, which is one more reason this
  model must never be used as an entropy source.

## Adding a capture

Record with `infnoise --raw`, then:

    ./replay --expect pass --fidelity yourfile.bin

Raw mode writes the bit stream the health check saw, packed MSB first, so no
conversion is needed.
