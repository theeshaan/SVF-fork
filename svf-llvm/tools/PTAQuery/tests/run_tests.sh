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

run_case_regex() {
  local case_name="$1"
  shift
  local src="$TEST_DIR/$case_name.c"
  local bc="$WORK_DIR/$case_name.bc"
  local out="$WORK_DIR/$case_name.out"

  "$CLANG_BIN" -emit-llvm -c -g -fno-discard-value-names "$src" -o "$bc"
  "$CXT_PTS_BIN" "$bc" > "$out"

  for expected_re in "$@"; do
    if ! grep -Eq "$expected_re" "$out"; then
      echo "case '$case_name' failed: expected regex '$expected_re'" >&2
      echo "--- actual output ---" >&2
      cat "$out" >&2
      exit 1
    fi
  done

  echo "PASS $case_name"
}

run_case simple_assign \
  "4 p_main a_main" \
  "6 q_main b_main" \
  "7 q_main a_main"

run_case call_context \
  "1 s_foo a_main b_main" \
  "16 pa_main a_main b_main" \
  "17 pb_main a_main b_main"

run_case function_pointer_struct \
  "19 fp.func1_main exampleFunction1" \
  "20 fp.func2_main exampleFunction2"

run_case array_locals \
  "5 arr.field0.field0_main a_main" \
  "6 arr.field0.field1_main b_main" \
  "7 p_main a_main b_main"

run_case struct_array_fields \
  "10 box.slots_main a_main" \
  "11 box.field0.field1_main b_main" \
  "12 p_main b_main"

run_case global_pointer \
  "6 gp g"

run_case branchy_if_chain \
  "10 p_main a_main" \
  "12 p_main b_main" \
  "14 p_main c_main" \
  "16 p_main d_main" \
  "18 p_main e_main" \
  "20 p_main f_main"

run_case cxt_merge_paths \
  "8 pA_main a_main" \
  "9 pB_main b_main" \
  "10 pC_main c_main" \
  "13 pA_main b_main c_main" \
  "15 pD_main a_main b_main c_main" \
  "15 pA_main a_main b_main c_main"

run_case_regex cxt_crash_regression \
  "^25 b_main call[0-9]+_main$" \
  "^26 c_main (call[0-9]+_main|\(empty\))$"

echo "All cxt-pts tests passed."
