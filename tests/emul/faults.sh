#!/bin/sh
# Fault matrix for the fake libftdi.  Each case asserts both that the call
# fails and that libinfnoise reports the specific documented diagnostic, so a
# fault cannot pass by failing for the wrong reason.
#
# Usage: ./faults.sh [path-to-devtest]
set -u

DEVTEST=${1:-./devtest}
TIMEOUT="timeout 60"
HANG_TIMEOUT="timeout 10"
fails=0

# expect_fail <name> <wanted message> <env assignments...> -- <args...>
expect_fail() {
    name=$1; want=$2; shift 2
    env_args=""
    while [ "$1" != "--" ]; do env_args="$env_args $1"; shift; done
    shift
    out=$(env $env_args $TIMEOUT "$DEVTEST" "$@" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        printf 'FAIL %-14s succeeded, expected failure\n' "$name"
        fails=$((fails + 1))
    elif ! printf '%s' "$out" | grep -q "$want"; then
        printf 'FAIL %-14s failed but message was: %s\n' "$name" "$out"
        fails=$((fails + 1))
    else
        printf 'ok   %-14s %s\n' "$name" "$want"
    fi
}

# expect_ok <name> <env assignments...> -- <args...>
expect_ok() {
    name=$1; shift
    env_args=""
    while [ "$1" != "--" ]; do env_args="$env_args $1"; shift; done
    shift
    if out=$(env $env_args $TIMEOUT "$DEVTEST" "$@" 2>&1 >/dev/null); then
        printf 'ok   %-14s\n' "$name"
    else
        printf 'FAIL %-14s %s\n' "$name" "$out"
        fails=$((fails + 1))
    fi
}

# expect_hang <name> <env assignments...> -- <args...>
# Pins the initInfnoise() defect described in README.md: with a source that
# never reaches on-target entropy, or a device that vanishes mid-warmup, the
# warmup loop neither enforces maxWarmupRounds nor checks readData's return,
# so it spins forever.  timeout's 124 is the expected outcome today.  When
# upstream bounds that loop these three flip to expect_fail.
expect_hang() {
    name=$1; shift
    env_args=""
    while [ "$1" != "--" ]; do env_args="$env_args $1"; shift; done
    shift
    env $env_args $HANG_TIMEOUT "$DEVTEST" "$@" >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 124 ]; then
        printf 'ok   %-14s hangs in initInfnoise, as documented\n' "$name"
    else
        printf 'FAIL %-14s expected hang (124), got %d: upstream may be fixed,\n' "$name" $rc
        printf '     %-14s see "Upstream defect found" in README.md\n' ""
        fails=$((fails + 1))
    fi
}

echo "-- healthy paths"
expect_ok   healthy      -- run 4096
expect_ok   serial       INFNOISE_FAKE_SERIALS=AAA111 -- open AAA111

echo "-- enumeration"
expect_fail no-devices   "Can't open"       INFNOISE_FAKE_DEVICES=0 -- open
expect_fail wrong-serial "Can't find"       INFNOISE_FAKE_SERIALS=AAA111 -- open NOPE-9999

echo "-- setup faults"
expect_fail open_fail     "Can't open"      INFNOISE_FAKE_FAULT=open_fail -- open
expect_fail baudrate_fail "baud rate"       INFNOISE_FAKE_FAULT=baudrate_fail -- open
expect_fail bitmode_fail  "bit-bang"        INFNOISE_FAKE_FAULT=bitmode_fail -- open
expect_fail write_fail    "USB write failed" INFNOISE_FAKE_FAULT=write_fail -- open
expect_fail read_fail     "USB read failed" INFNOISE_FAKE_FAULT=read_fail -- open
expect_fail short_read    "USB read failed" INFNOISE_FAKE_FAULT=short_read -- open

echo "-- gain drift leaves the entropy band"
expect_fail k-high       "no on-target entropy" INFNOISE_FAKE_K=1.8952 -- run 4096
expect_fail k-low        "no on-target entropy" INFNOISE_FAKE_K=1.7848 -- run 4096

echo "-- known hangs, see README"
expect_hang stuck-comp   INFNOISE_FAKE_STUCK=0 -- run 4096
expect_hang comp-bias    INFNOISE_FAKE_U=0.05 -- run 4096
expect_hang unplug       INFNOISE_FAKE_FAULT=unplug INFNOISE_FAKE_FAULT_AT=5 -- run 4096

if [ $fails -ne 0 ]; then
    echo "$fails case(s) failed"
    exit 1
fi
echo "all fault cases behaved as documented"
