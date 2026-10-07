#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run-perf.sh DATA_DIR [RESULTS_DIR]

Runs one-factor-at-a-time perf experiments for each selected kernel. Each
experiment starts from BASE_* values and varies only one setting. DATA_DIR
must contain generated *_f32.floatdata files (or *_f64.floatdata when
ELEMENT_TYPE=double). Build with matching `make ELEMENT_TYPE=...` first.
Results are written to a timestamped directory by default.

Optional environment variables (comma-separated lists):
  ELEMENT_TYPE=float|double
  SIZES=65536,1048576
  THREADS=1,2,4
  STRIDES=1,16
  PATTERNS=stride,random
  KERNELS=reduction,memset,vector-multiply,scalar-multiply
  PROFILES=cache,tlb
  REPEATS=1
  BASE_SIZE=4194304 BASE_THREADS=1 BASE_STRIDE=1 BASE_PATTERN=stride
  PERF_EVENTS=task-clock (software-only default to avoid PMU contention)
USAGE
}

if [[ ${1:-} == -h || ${1:-} == --help ]]; then
  usage
  exit 0
fi
if (($# < 1 || $# > 2)); then
  usage >&2
  exit 2
fi

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
DATA_DIR=$(cd -- "$1" 2>/dev/null && pwd) || { printf 'Data directory not found: %s\n' "$1" >&2; exit 2; }
ELEMENT_TYPE=${ELEMENT_TYPE:-float}
case "$ELEMENT_TYPE" in
  float) suffix=f32 ;;
  double) suffix=f64 ;;
  *) printf 'ELEMENT_TYPE must be float or double\n' >&2; exit 2 ;;
esac

SIZES=${SIZES:-1024,16384,262144,4194304,16777216}
THREADS=${THREADS:-1,2,4}
STRIDES=${STRIDES:-1,16,32,64,128}
PATTERNS=${PATTERNS:-stride,random}
KERNELS=${KERNELS:-reduction,memset,vector-multiply,scalar-multiply}
REPEATS=${REPEATS:-5}
BASE_SIZE=${BASE_SIZE:-4194304}
BASE_THREADS=${BASE_THREADS:-1}
BASE_STRIDE=${BASE_STRIDE:-1}
BASE_PATTERN=${BASE_PATTERN:-stride}
PERF_EVENTS=${PERF_EVENTS:-task-clock}
PROFILES=${PROFILES:-cache,tlb}
RESULTS_DIR=${2:-"$ROOT/results/perf/$(date +%Y%m%d-%H%M%S)"}

for setting in BASE_SIZE BASE_THREADS BASE_STRIDE; do
  value=${!setting}
  if ! [[ $value =~ ^[1-9][0-9]*$ ]]; then
    printf '%s must be a positive integer\n' "$setting" >&2
    exit 2
  fi
done
if [[ $BASE_PATTERN != stride && $BASE_PATTERN != random ]]; then
  printf 'BASE_PATTERN must be stride or random\n' >&2
  exit 2
fi
if ! [[ $REPEATS =~ ^[1-9][0-9]*$ ]]; then
  printf 'REPEATS must be a positive integer\n' >&2
  exit 2
fi
command -v perf >/dev/null || { printf 'perf was not found on PATH\n' >&2; exit 127; }

IFS=, read -r -a sizes <<< "$SIZES"
IFS=, read -r -a thread_counts <<< "$THREADS"
IFS=, read -r -a strides <<< "$STRIDES"
IFS=, read -r -a patterns <<< "$PATTERNS"
IFS=, read -r -a kernels <<< "$KERNELS"
IFS=, read -r -a profiles <<< "$PROFILES"
IFS=, read -r -a events <<< "$PERF_EVENTS"

for profile in "${profiles[@]}"; do
  case "$profile" in
    cache|tlb) ;;
    *) printf 'Unknown profile: %s (expected cache or tlb)\n' "$profile" >&2; exit 2 ;;
  esac
done
for kernel in "${kernels[@]}"; do
  case "$kernel" in
    reduction|memset|vector-multiply|scalar-multiply) ;;
    *) printf 'Unknown kernel: %s\n' "$kernel" >&2; exit 2 ;;
  esac
  [[ -x "$ROOT/build/experiments/$kernel" ]] || {
    printf 'Missing binary: %s (run make first)\n' "$ROOT/build/experiments/$kernel" >&2
    exit 2
  }
done

for name in a x y; do
  file="$DATA_DIR/${name}_${suffix}.floatdata"
  if [[ ! -f $file ]]; then
    # Generator files use the precision suffix after the array name.
    printf 'Required data file is missing: %s\n' "$file" >&2
    exit 2
  fi
done

run_experiment() {
  local kernel=$1 profile=$2 size=$3 thread_count=$4 stride=$5 pattern=$6 repeat=$7
  local binary="$ROOT/build/experiments/$kernel"
  local args=(--profile "$profile" --n "$size" --threads "$thread_count" --stride "$stride" --pattern "$pattern")
  case "$kernel" in
    reduction|memset) args+=(--a "$DATA_DIR/a_${suffix}.floatdata") ;;
    vector-multiply) args+=(--x "$DATA_DIR/x_${suffix}.floatdata" --y "$DATA_DIR/y_${suffix}.floatdata") ;;
    scalar-multiply) args+=(--x "$DATA_DIR/x_${suffix}.floatdata") ;;
  esac
  if [[ $kernel == memset ]]; then args+=(--fill "${FILL_VALUE:-1}"); fi
  if [[ $kernel == scalar-multiply ]]; then args+=(--scalar "${SCALAR_VALUE:-2}"); fi
  $binary "${args[@]}"
  
}

for kernel in "${kernels[@]}"; do
  for profile in "${profiles[@]}"; do
    for ((repeat=1; repeat<=REPEATS; repeat++)); do
      # Run the baseline, then vary one parameter at a time.
      run_experiment "$kernel" "$profile" "$BASE_SIZE" "$BASE_THREADS" "$BASE_STRIDE" "$BASE_PATTERN" "$repeat"
      for size in "${sizes[@]}"; do
        [[ $size == "$BASE_SIZE" ]] || run_experiment "$kernel" "$profile" "$size" "$BASE_THREADS" "$BASE_STRIDE" "$BASE_PATTERN" "$repeat"
      done
      for thread_count in "${thread_counts[@]}"; do
        [[ $thread_count == "$BASE_THREADS" ]] || run_experiment "$kernel" "$profile" "$BASE_SIZE" "$thread_count" "$BASE_STRIDE" "$BASE_PATTERN" "$repeat"
      done
      for stride in "${strides[@]}"; do
        [[ $stride == "$BASE_STRIDE" ]] || run_experiment "$kernel" "$profile" "$BASE_SIZE" "$BASE_THREADS" "$stride" "$BASE_PATTERN" "$repeat"
      done
      for pattern in "${patterns[@]}"; do
        [[ $pattern == "$BASE_PATTERN" ]] || run_experiment "$kernel" "$profile" "$BASE_SIZE" "$BASE_THREADS" "$BASE_STRIDE" "$pattern" "$repeat"
      done
    done
  done
done

