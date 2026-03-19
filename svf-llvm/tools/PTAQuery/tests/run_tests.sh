#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
CLANG_BIN="${CLANG_BIN:-$ROOT_DIR/llvm-18.1.0.obj/bin/clang}"
CXT_PTS_BIN="${CXT_PTS_BIN:-$ROOT_DIR/Release-build/bin/cxt-pts}"
TEST_DIR="$ROOT_DIR/svf-llvm/tools/PTAQuery/tests"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

if [[ ! -x "$CLANG_BIN" ]]; then
  echo "missing clang at $CLANG_BIN" >&2
  exit 1
fi

if [[ ! -x "$CXT_PTS_BIN" ]]; then
  echo "missing cxt-pts binary at $CXT_PTS_BIN" >&2
  exit 1
fi

run_case() {
  local case_name="$1"
  shift
  local src="$TEST_DIR/$case_name.c"
  local bc="$WORK_DIR/$case_name.bc"
  local out="$WORK_DIR/$case_name.out"

  "$CLANG_BIN" -emit-llvm -c -g -fno-discard-value-names "$src" -o "$bc"
  "$CXT_PTS_BIN" "$bc" > "$out"

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
  $'q_main\ta_main'

run_case call_context \
  $'pa_main\ta_main' \
  $'pb_main\tb_main'

run_case function_pointer_struct \
  $'fp.func1_main\texampleFunction1' \
  $'fp.func2_main\texampleFunction2'

run_case array_locals \
  $'arr.field0.field0_main\ta_main' \
  $'arr.field0.field1_main\tb_main' \
  $'p_main\tb_main'

run_case struct_array_fields \
  $'box.slots_main\ta_main' \
  $'box.field0.field1_main\tb_main' \
  $'p_main\ta_main'

run_case global_pointer \
  $'gp\tg'

run_case branchy_if_chain \
  $'p_main\ta_main, b_main, c_main, d_main, e_main, f_main'

echo "All cxt-pts tests passed."