CFLAGS ?= -O2 -Wall -Wextra
SOFTWARE = ../../software
SAMPLES = ../samples

# healthcheck.c references no libftdi symbol, but libinfnoise.h embeds
# struct ftdi_context in struct infnoise_context, so the headers are needed.
FTDI_CFLAGS := $(shell pkg-config --cflags libftdi1 2>/dev/null)

# Same byte count as the committed sample, so the control is a fair comparison.
UNIFORM_BYTES = 1216512

all: replay model

replay: replay.c $(SOFTWARE)/healthcheck.c
	$(CC) $(CFLAGS) -I$(SOFTWARE) $(FTDI_CFLAGS) -o $@ $^ -lm

model: model.c
	$(CC) $(CFLAGS) -o $@ $< -lm

check: replay model
	./replay --expect pass --fidelity $(SAMPLES)/infnoise_raw.bin
	./replay --expect fail --uniform $(UNIFORM_BYTES)
	./replay --expect fail $(SAMPLES)/infnoise_whitened.bin
	./model | ./replay --expect pass --fidelity -
	./model --k 1.8952 --bits 2000000 | ./replay --expect fail -
	./model --k 1.7848 --bits 2000000 | ./replay --expect fail -
	./model --u 0.05 --bits 2000000 | ./replay --expect fail -
	./model --stuck 0 --bits 1000000 | ./replay --expect fail - 2>/dev/null

clean:
	rm -f replay model

.PHONY: all check clean
