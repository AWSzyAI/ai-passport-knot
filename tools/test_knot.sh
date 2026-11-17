#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir="$(mktemp -d /tmp/knot-tests.XXXXXX)"
echo '[1/3] Time model'
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_knot_model.c main/knot_model.c -o "$test_dir/model"
"$test_dir/model"
echo '[2/3] Persistent event store (fault injection)'
cc -std=c11 -Wall -Wextra -Werror -Itests/knot_stubs -Imain tests/test_knot_store.c main/knot_store.c main/knot_model.c -o "$test_dir/store"
"$test_dir/store"
echo '[3/3] Receiver and simulated BLE transport'
python3 -m unittest discover -s receiver -p 'test_*.py' -v
