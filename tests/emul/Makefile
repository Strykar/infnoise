CFLAGS ?= -O2 -Wall -Wextra
SOFTWARE = ../../software
KECCAK = $(SOFTWARE)/Keccak
SAMPLES = ../samples

# healthcheck.c references no libftdi symbol, but libinfnoise.h embeds
# struct ftdi_context in struct infnoise_context, so the headers are needed.
# fake_libftdi.c needs them for the same reason: it fakes the functions and
# must use the real types.
FTDI_CFLAGS := $(shell pkg-config --cflags libftdi1 2>/dev/null)

# Same byte count as the committed sample, so the control is a fair comparison.
UNIFORM_BYTES = 1216512

# Upstream sources linked into devtest, unmodified.
LIBSRC = $(SOFTWARE)/libinfnoise.c $(SOFTWARE)/healthcheck.c \
         $(SOFTWARE)/writeentropy.c $(KECCAK)/KeccakF-1600-reference.c

# initInfnoise() can block forever (see README, "Upstream defect found"), so
# every devtest invocation is bounded.
TIMEOUT = timeout 60
HANG_TIMEOUT = timeout 10

all: replay model devtest

replay: replay.c $(SOFTWARE)/healthcheck.c
	$(CC) $(CFLAGS) -I$(SOFTWARE) $(FTDI_CFLAGS) -o $@ $^ -lm

model: model.c source_model.h
	$(CC) $(CFLAGS) -o $@ model.c -lm

devtest: devtest.c fake_libftdi.c source_model.h $(LIBSRC)
	$(CC) $(CFLAGS) -I$(SOFTWARE) -I$(KECCAK) $(FTDI_CFLAGS) -o $@ \
		devtest.c fake_libftdi.c $(LIBSRC) -lm -lrt

check: check-replay check-model check-device

# Stage 0: the health check judged against recorded reality.
check-replay: replay
	./replay --expect pass --fidelity $(SAMPLES)/infnoise_raw.bin
	./replay --expect fail --uniform $(UNIFORM_BYTES)
	./replay --expect fail $(SAMPLES)/infnoise_whitened.bin

# Stage 1: the source model, judged by the same gates plus fault knobs.
check-model: replay model
	./model | ./replay --expect pass --fidelity -
	./model --k 1.8952 --bits 2000000 | ./replay --expect fail -
	./model --k 1.7848 --bits 2000000 | ./replay --expect fail -
	./model --u 0.05 --bits 2000000 | ./replay --expect fail -
	./model --stuck 0 --bits 1000000 | ./replay --expect fail - 2>/dev/null

# Stage 2: unmodified libinfnoise driven through the fake libftdi.
check-device: devtest replay
	@echo "-- enumeration, including decoys sharing 0403:6015"
	$(TIMEOUT) ./devtest list | grep -q "Infinite Noise TRNG"
	INFNOISE_FAKE_DEVICES=2 INFNOISE_FAKE_DECOYS=2 \
		INFNOISE_FAKE_SERIALS=AAA111,BBB222 $(TIMEOUT) ./devtest list \
		| grep -c . | grep -qx 4
	@echo "-- the real library's read path over the model"
	$(TIMEOUT) ./devtest raw 1200000 | ./replay --expect pass --fidelity -
	@echo "-- fault matrix"
	./faults.sh ./devtest

clean:
	rm -f replay model devtest

.PHONY: all check check-replay check-model check-device clean
