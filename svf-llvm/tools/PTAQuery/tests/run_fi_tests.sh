#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
CLANG_BIN="${CLANG_BIN:-$ROOT_DIR/llvm-18.1.0.obj/bin/clang}"
FI_PTS_BIN="${FI_PTS_BIN:-$ROOT_DIR/Release-build/bin/fi-pts}"
TEST_DIR="$ROOT_DIR/svf-llvm/tools/PTAQuery/tests"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

if [[ ! -x "$CLANG_BIN" ]]; then
  echo "missing clang at $CLANG_BIN" >&2
  exit 1
fi

if [[ ! -x "$FI_PTS_BIN" ]]; then
  echo "missing fi-pts binary at $FI_PTS_BIN" >&2
  exit 1
fi

run_case() {
  local case_name="$1"
  shift
  local src="$TEST_DIR/$case_name.c"
  local bc="$WORK_DIR/$case_name.bc"
  local out="$WORK_DIR/$case_name.out"

  "$CLANG_BIN" -emit-llvm -c -g -fno-discard-value-names "$src" -o "$bc"
  "$FI_PTS_BIN" "$bc" > "$out"

  for expected in "$@"; do
    if ! grep -Fqx "$expected" "$out"; then
      echo "case '$case_name' failed: expected line '$expected'" >&2
      echo "--- actual output ---" >&2
      cat "$out" >&2
      exit 1
    fi
  done

  echo "PASS $case_name"
}

run_case simple_assign \
  $'p_main\ta_main' \
  $'q_main\ta_main, b_main'

run_case call_context \
  $'pa_main\ta_main, b_main' \
  $'pb_main\ta_main, b_main'

run_case function_pointer_struct \
  $'fp.func1_main\texampleFunction1' \
  $'fp.func2_main\texampleFunction2'

run_case branchy_if_chain \
  $'p_main\ta_main, b_main, c_main, d_main, e_main, f_main'

echo "All fi-pts tests passed."
