# Health check replay

Replays a raw capture through `software/healthcheck.c` and checks the verdict.

    make check

This runs three cases against the captures already in `tests/samples`:

| input | expected |
|---|---|
| `infnoise_raw.bin` | PASS |
| seeded uniform random | FAIL |
| `infnoise_whitened.bin` | FAIL |

No hardware and no privileges. The health check is pure computation over the bit
stream, so a recorded capture exercises it exactly as the device would.

## Why uniform random has to fail

The health check requires measured entropy within `INM_ACCURACY` of
`log2(DESIGN_K)`, which is a two-sided band: 0.854 to 0.906 bits per bit. It also
rejects runs longer than `INM_MAX_SEQUENCE`.

Uniform random measures about 1.0 bits per bit, so it fails the upper bound, and
it produces runs past 20. Both failures are real and independent. Anything that
fakes this device by emitting uniform bits is rejected by the device's own health
check, so the negative controls are part of the contract rather than a curiosity.

Measured on the committed captures:

| input | entropy/bit | K | longest run | verdict |
|---|---|---|---|---|
| `infnoise_raw.bin` | 0.8790 | 1.8391 | 6 | PASS |
| uniform random | 1.0023 | 2.0032 | 22 | FAIL |
| `infnoise_whitened.bin` | 1.0023 | 2.0031 | 23 | FAIL |

The whitened capture is Keccak output, so it measures like uniform random. That
is correct behaviour, not a defect: whitened data is not meant to be fed back
through the health check.

## Adding a capture

Record with `infnoise --raw`, then:

    ./replay --expect pass yourfile.bin

Raw mode writes the bit stream the health check saw, packed MSB first, so no
conversion is needed.
