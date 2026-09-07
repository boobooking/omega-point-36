#!/bin/sh
# Run the keymap against ZMK's native_posix simulator, in Docker.
#
# The simulator builds the firmware as a Linux binary with a mock key scanner
# that replays timed press/release events, so hold-tap decisions, layer
# resolution, combos and macro timing can be checked deterministically. It
# cannot model the split: there is one node, no peripheral half and no BLE, so
# anything caused by event ordering between the halves is out of its reach.
#
#   ./tests/run.sh                            run every case
#   ./tests/run.sh cmd-space-in-order         run one
#   ACCEPT=1 ./tests/run.sh cmd-space-...     overwrite the snapshot
#
# Cases build one at a time; four parallel Zephyr builds in one container run
# out of memory and report as "did not build". Override with J=<n> if you have
# the headroom.
#
# The first run clones ZMK and its west dependencies into $WS (about 1.5 GB)
# and takes several minutes; later runs reuse it. The tag has to match what the
# firmware ships: ergohaven-zmk's manifest pins zmkfirmware/zmk at v0.3.0, so
# that is what gets cloned here. Bump both together or the simulator stops
# answering questions about the firmware that actually runs.
set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
WS=${WS:-${TMPDIR:-/tmp}/zmk-sim-ws}
IMAGE=zmkfirmware/zmk-build-arm:stable

if [ ! -d "$WS/zmk/zephyr" ]; then
    echo "Setting up the ZMK workspace in $WS (one-time, ~1.5 GB)"
    mkdir -p "$WS"
    docker run --rm -v "$WS":/ws -w /ws "$IMAGE" sh -c '
        git clone --depth 1 --branch v0.3.0 https://github.com/zmkfirmware/zmk.git zmk &&
        cd zmk && west init -l app &&
        west update --narrow -o=--depth=1 && west zephyr-export'
fi

exec docker run --rm \
    -v "$WS":/ws \
    -v "$REPO/config":/cfg:ro \
    -v "$REPO/tests":/tests \
    -w /ws/zmk/app \
    -e ZMK_TESTS_AUTO_ACCEPT="${ACCEPT:+1}" \
    -e J="${J:-1}" \
    "$IMAGE" sh -c "./run-test.sh /tests${1:+/$1}"
