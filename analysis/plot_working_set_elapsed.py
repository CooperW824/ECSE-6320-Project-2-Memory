#!/usr/bin/env python3
"""Plot median and minimum elapsed time by working-set size."""

from __future__ import annotations

import argparse
import re
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


FIELD_PATTERN = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)")


def load_elapsed_times(path: Path) -> dict[int, list[float]]:
    """Read elapsed_ms samples grouped by working-set size (elements)."""
    samples: dict[int, list[float]] = defaultdict(list)
    with path.open(encoding="utf-8") as input_file:
        for line in input_file:
            fields = dict(FIELD_PATTERN.findall(line))
            try:
                elements = int(fields["elements"])
                elapsed_ms = float(fields["elapsed_ms"])
            except (KeyError, ValueError):
                continue
            samples[elements].append(elapsed_ms)

    if not samples:
        raise SystemExit(f"No valid working-set elapsed-time records found in {path}")
    return samples


def plot_elapsed_times(samples: dict[int, list[float]], output_path: Path) -> None:
    sizes = sorted(samples)
    medians = [statistics.median(samples[size]) for size in sizes]
    minimums = [min(samples[size]) for size in sizes]

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(sizes, medians, marker="o", label="Median elapsed time")
    ax.plot(sizes, minimums, marker="s", label="Minimum elapsed time")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Working-set size (elements)")
    ax.set_ylabel("Elapsed time (ms)")
    ax.set_title("Elapsed Time vs. Working-Set Size")
    ax.grid(True, which="both", linestyle="--", alpha=0.35)
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    repository_dir = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Plot median and minimum elapsed time by working-set size."
    )
    parser.add_argument(
        "--results-file",
        type=Path,
        default=repository_dir / "results" / "working-set-sweep.txt",
        help="Benchmark log file (default: %(default)s)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent / "working_set_elapsed_time.png",
        help="Output chart path (default: %(default)s)",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    plot_elapsed_times(load_elapsed_times(args.results_file), args.output)
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
