# Memory experiment runner

From the repository root, build all four kernel variants with:

```sh
make
```

The binaries are written to `build/experiments/`. Use `make ELEMENT_TYPE=double`
to build them for double-precision inputs. Alternatively, build a single
variant from this directory, selecting exactly one kernel at compile time:

```sh
gcc -O2 -std=c11 -Wall -Wextra -pthread -DDEFINE=REDUCTION main.c kernels.c perf.c -o experiment
```

Supported `DEFINE` values are `REDUCTION`, `MEMSET`, `VECTOR_MULTIPLY`, and
`SCALAR_MULTIPLY`. The default element type is `float`; add
`-DELEMENT_TYPE_DOUBLE` to build for `double` inputs (the matching generator
files are `*_f64.floatdata`).

The runner requires `--profile cache|tlb` to select its event set. Cache mode
initializes L1/L2/LLC counters; TLB mode initializes only DTLB counters. It
also accepts `--n ELEMENTS`, `--threads N`, `--stride N`, and
`--pattern stride|random`. The working set is `n` logical elements; for stride
`s`, the accessed file indices range through `(n - 1) * s`. Input files must
contain at least that many elements. Random mode shuffles the selected strided
positions using `--seed` (default 1). Thread ranges are disjoint, so each
output position is written by only one worker.

Provide input paths for the selected kernel:

```sh
./experiment --profile cache --a data/a_f32.floatdata --n 1048576 --threads 8
./experiment --profile tlb --x data/x_f32.floatdata --y data/y_f32.floatdata \
  --n 1048576 --stride 4 --pattern random --threads 8 --seed 17
```

Reduction requires `--a`; memset also takes `--a` to establish the available
array size and writes to a separate in-memory output buffer. Vector
multiplication requires `--x` and `--y`; scalar-vector multiplication
requires `--x`. `--fill VALUE` configures memset (default `0`) and
`--scalar VALUE` configures scalar-vector multiplication (default `1`). Both
are parsed as runtime options. The runner reports elapsed milliseconds, selected
parameters, and the reduction sum when applicable.

Cache mode reports L1 load, L2, and LLC miss rates; its L1 load rate is `NA`
for memset because that kernel performs no loads. TLB mode reports the DTLB
miss rate.

Generate input files with the project utility, for example:

```sh
gcc -O2 -std=c11 ../data-gen/main.c -o gen_floatdata
./gen_floatdata 1048576 data
```

## Perf sweep

After building the experiments and generating data, run the `perf stat` sweep
from the repository root:

```sh
scripts/run-perf.sh data
```

By default, it sweeps both cache and TLB profiles, both access patterns, strides
1 and 16, working sets of
65,536 and 1,048,576 elements, thread counts 1/2/4, and all four kernels. Each
run has a separate `perf` output file and is indexed in `manifest.csv` under
`results/perf/<timestamp>/`. The sweep dimensions, event list, repeat count,
and data precision can be changed through the environment; set `PROFILES=cache`
or `PROFILES=tlb` to run only one profile. The wrapper defaults to the software
`task-clock` event to avoid PMU contention. `PERF_EVENTS` can override it; see
`scripts/run-perf.sh --help` for the variables and defaults. For example:

```sh
SIZES=65536 THREADS=1,8 STRIDES=1 PATTERNS=stride REPEATS=3 \
  scripts/run-perf.sh data results/perf/quick-check
```

Input files must cover the largest selected strided range: at least
`(max(SIZES) - 1) * max(STRIDES) + 1` elements. Thus the default sweep needs
16,777,201 elements in each input file. Build with `make ELEMENT_TYPE=double`
when setting `ELEMENT_TYPE=double` for the sweep.
