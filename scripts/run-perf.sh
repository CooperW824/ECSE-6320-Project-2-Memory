#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run-perf.sh DATA_DIR [RESULTS_DIR]

Runs perf stat for each selected kernel and parameter combination. DATA_DIR
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
  REPEATS=1
  PERF_EVENTS=cycles,instructions,cache-references,cache-misses,dTLB-loads,dTLB-load-misses
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
PERF_EVENTS=${PERF_EVENTS:-cycles,instructions,cache-references,cache-misses,dTLB-loads,dTLB-load-misses}
RESULTS_DIR=${2:-"$ROOT/results/perf/$(date +%Y%m%d-%H%M%S)"}

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
IFS=, read -r -a events <<< "$PERF_EVENTS"

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

mkdir -p -- "$RESULTS_DIR"
RESULTS_DIR=$(cd -- "$RESULTS_DIR" && pwd)
manifest="$RESULTS_DIR/manifest.csv"
printf 'kernel,size,threads,stride,pattern,repeat,perf_log\n' > "$manifest"
run_number=0

for kernel in "${kernels[@]}"; do
  binary="$ROOT/build/experiments/$kernel"
  for size in "${sizes[@]}"; do
    for thread_count in "${thread_counts[@]}"; do
      for stride in "${strides[@]}"; do
        for pattern in "${patterns[@]}"; do
          for ((repeat=1; repeat<=REPEATS; repeat++)); do
            ((run_number+=1))
            args=(--n "$size" --threads "$thread_count" --stride "$stride" --pattern "$pattern")
            case "$kernel" in
              reduction|memset) args+=(--a "$DATA_DIR/a_${suffix}.floatdata") ;;
              vector-multiply) args+=(--x "$DATA_DIR/x_${suffix}.floatdata" --y "$DATA_DIR/y_${suffix}.floatdata") ;;
              scalar-multiply) args+=(--x "$DATA_DIR/x_${suffix}.floatdata") ;;
            esac
            if [[ $kernel == memset ]]; then args+=(--fill "${FILL_VALUE:-1}"); fi
            if [[ $kernel == scalar-multiply ]]; then args+=(--scalar "${SCALAR_VALUE:-2}"); fi
            label=$(printf '%04d-%s-n%s-t%s-s%s-%s-r%s' \
              "$run_number" "$kernel" "$size" "$thread_count" "$stride" "$pattern" "$repeat")
            log="$RESULTS_DIR/$label.perf.txt"
            printf '[%s] perf stat -> %s\n' "$label" "$log"
            perf stat -x, -e "$(IFS=,; printf '%s' "${events[*]}")" -o "$log" -- "$binary" "${args[@]}"
            printf '%s,%s,%s,%s,%s,%s,%s\n' \
              "$kernel" "$size" "$thread_count" "$stride" "$pattern" "$repeat" "$label.perf.txt" >> "$manifest"
          done
        done
      done
    done
  done
done

printf 'Completed %s runs. Manifest: %s\n' "$run_number" "$manifest"
