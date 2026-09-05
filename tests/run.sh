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
# The first run clones ZMK and its west dependencies into $WS (about 1.5 GB)
# and takes several minutes; later runs reuse it.
set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
WS=${WS:-${TMPDIR:-/tmp}/zmk-sim-ws}
IMAGE=zmkfirmware/zmk-build-arm:stable

if [ ! -d "$WS/zmk/zephyr" ]; then
    echo "Setting up the ZMK workspace in $WS (one-time, ~1.5 GB)"
    mkdir -p "$WS"
    docker run --rm -v "$WS":/ws -w /ws "$IMAGE" sh -c '
        git clone --depth 1 https://github.com/ergohaven/zmk.git zmk &&
        cd zmk && west init -l app &&
        west update --narrow -o=--depth=1 && west zephyr-export'
fi

exec docker run --rm \
    -v "$WS":/ws \
    -v "$REPO/config":/cfg:ro \
    -v "$REPO/tests":/tests \
    -w /ws/zmk/app \
    -e ZMK_TESTS_AUTO_ACCEPT="${ACCEPT:+1}" \
    "$IMAGE" sh -c "./run-test.sh /tests${1:+/$1}"
