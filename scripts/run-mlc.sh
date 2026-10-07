#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run-mlc.sh [RESULTS_DIR]

Collects Intel Memory Latency Checker (mlc) latency and bandwidth data. Each
measurement is saved as a separate raw output file, with commands and labels
recorded in manifest.tsv. The default output directory is
results/mlc/<timestamp>.

Measurements:
  - Idle latency using working sets sized for L1, L2, and LLC cache samples
  - Loaded latency/bandwidth for 64B, 256B, 512B, and 1024B strides
  - Loaded latency/bandwidth with random latency and bandwidth accesses
  - Loaded latency/bandwidth for 100% read, 50/50, 2:1 read/write, and
    write-only traffic
  - Peak injection bandwidth across MLC's supported read/write ratios

Environment variables:
  MLC_BIN=mlc          MLC executable (default: mlc on PATH)
  MLC_SECONDS=2        Seconds per measurement
  CACHE_L1_KB=32       Cache working-set sizes in KiB
  CACHE_L2_KB=256
  CACHE_LLC_KB=8192
  MLC_BUFFER_KB=100000 Optional buffer size override for non-cache tests

The cache working-set defaults are examples; set them to fit the cache sizes
of the machine being measured. MLC may require elevated privileges for full
hardware-counter or prefetcher control.
USAGE
}

if [[ ${1:-} == -h || ${1:-} == --help ]]; then
  usage
  exit 0
fi
if (($# > 1)); then
  usage >&2
  exit 2
fi

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MLC_BIN=${MLC_BIN:-mlc}
MLC_SECONDS=${MLC_SECONDS:-2}
CACHE_L1_KB=${CACHE_L1_KB:-32}
CACHE_L2_KB=${CACHE_L2_KB:-256}
CACHE_LLC_KB=${CACHE_LLC_KB:-8192}
MLC_BUFFER_KB=${MLC_BUFFER_KB:-100000}
RESULTS_DIR=${1:-"$ROOT/results/mlc/$(date +%Y%m%d-%H%M%S)"}

if ! command -v "$MLC_BIN" >/dev/null 2>&1; then
  printf 'MLC executable was not found: %s\n' "$MLC_BIN" >&2
  exit 127
fi
for setting in MLC_SECONDS CACHE_L1_KB CACHE_L2_KB CACHE_LLC_KB MLC_BUFFER_KB; do
  value=${!setting}
  if ! [[ $value =~ ^[1-9][0-9]*$ ]]; then
    printf '%s must be a positive integer\n' "$setting" >&2
    exit 2
  fi
done

mkdir -p -- "$RESULTS_DIR"
RESULTS_DIR=$(cd -- "$RESULTS_DIR" && pwd)
MANIFEST="$RESULTS_DIR/manifest.tsv"
printf 'label\tfile\tcommand\n' > "$MANIFEST"

run_mlc() {
  local label=$1
  shift
  local file="$RESULTS_DIR/${label}.txt"
  local -a command=("$MLC_BIN" "$@")
  printf 'Running %-38s -> %s\n' "$label" "$file"
  printf '%s\t%s\t' "$label" "${file##*/}" >> "$MANIFEST"
  printf '%q ' "${command[@]}" >> "$MANIFEST"
  printf '\n' >> "$MANIFEST"
  if ! "${command[@]}" > "$file" 2>&1; then
    printf 'MLC failed for %s; see %s\n' "$label" "$file" >&2
    return 1
  fi
}

# Restrict the idle-latency working set to each configured cache sample.
run_mlc "idle-latency-l1-${CACHE_L1_KB}kib" --idle_latency -r "-b${CACHE_L1_KB}" "-t${MLC_SECONDS}"
run_mlc "idle-latency-l2-${CACHE_L2_KB}kib" --idle_latency -r "-b${CACHE_L2_KB}" "-t${MLC_SECONDS}"
run_mlc "idle-latency-llc-${CACHE_LLC_KB}kib" --idle_latency -r "-b${CACHE_LLC_KB}" "-t${MLC_SECONDS}"

# Sequential access at the requested byte strides. The loaded-latency mode
# reports latency while sweeping generated memory bandwidth.
for stride in 64 256 512 1024; do
  run_mlc "loaded-latency-stride-${stride}b" --loaded_latency "-l${stride}" "-b${MLC_BUFFER_KB}" "-t${MLC_SECONDS}"
done

# Randomize both the latency pointer chase (-r) and bandwidth traffic (-U).
run_mlc "loaded-latency-random" --loaded_latency -r -U "-b${MLC_BUFFER_KB}" "-t${MLC_SECONDS}"

# Read-only (-R), 2:1 read/write (-W2), 1:1 read/write (-W5), and
# write-only non-temporal traffic (-W6), as supported by MLC.
run_mlc "loaded-latency-mix-100read" --loaded_latency -R "-l64" "-b${MLC_BUFFER_KB}" "-t${MLC_SECONDS}"
run_mlc "loaded-latency-mix-66read-33write" --loaded_latency -W2 "-l64" "-b${MLC_BUFFER_KB}" "-t${MLC_SECONDS}"
run_mlc "loaded-latency-mix-50read-50write" --loaded_latency -W5 "-l64" "-b${MLC_BUFFER_KB}" "-t${MLC_SECONDS}"
run_mlc "loaded-latency-mix-100write" --loaded_latency -W6 "-l64" "-b${MLC_BUFFER_KB}" "-t${MLC_SECONDS}"

# Peak injection bandwidth reports MLC's supported ratios in one scan; unlike
# loaded_latency, this mode does not accept -W or -R selectors.
run_mlc "peak-bandwidth-ratios" --peak_injection_bandwidth "-b${MLC_BUFFER_KB}" "-t${MLC_SECONDS}"


printf 'MLC results and manifest written to %s\n' "$RESULTS_DIR"
