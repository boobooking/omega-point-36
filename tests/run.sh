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

# en_letters is a hand-kept copy of en, and numbers_ru of numbers_en; nothing in
# the devicetree enforces either.
python3 "$REPO/tests/check-en-letters.py"
python3 "$REPO/tests/check-numbers-ru.py"

# The resync state machine and adapter core are plain C with no Zephyr in them,
# so they run here rather than in the simulator, which has no BLE to exercise
# them with.
cc -std=c11 -Wall -Wextra -Werror -o "${TMPDIR:-/tmp}/resync_test" \
    "$REPO/module/src/resync_state.c" "$REPO/tests/resync-state/test_resync_state.c"
"${TMPDIR:-/tmp}/resync_test" > /dev/null

cc -std=c11 -Wall -Wextra -Werror -o "${TMPDIR:-/tmp}/resync_adapter_test" \
    "$REPO/module/src/resync_state.c" "$REPO/module/src/resync_adapter.c" \
    "$REPO/tests/resync-state/test_resync_adapter.c"
"${TMPDIR:-/tmp}/resync_adapter_test" > /dev/null

exec docker run --rm \
    -v "$WS":/ws \
    -v "$REPO/config":/cfg:ro \
    -v "$REPO/tests":/tests \
    -w /ws/zmk/app \
    -e ZMK_TESTS_AUTO_ACCEPT="${ACCEPT:+1}" \
    -e J="${J:-1}" \
    "$IMAGE" sh -c "./run-test.sh /tests${1:+/$1}"
