#!/usr/bin/env python3
"""Plot cache and TLB miss counts from the small-page perf results."""

from __future__ import annotations

import argparse
import csv
import re
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt


WORKING_SET_SIZE = 4_194_304
METRICS = ("cache-misses", "dTLB-load-misses")
KERNELS = ("reduction", "memset", "vector-multiply", "scalar-multiply")
FILENAME_PATTERN = re.compile(
    r"\d+-(?P<kernel>.+?)-n(?P<n>\d+)-t(?P<threads>\d+)"
    r"-s(?P<stride>\d+)-(?P<pattern>stride|random)-r(?P<repeat>\d+)\.perf\.txt$"
)


@dataclass(frozen=True)
class PerfResult:
    kernel: str
    working_set_size: int
    threads: int
    stride: int
    pattern: str
    repeat: int
    metrics: dict[str, int]


def parse_perf_file(path: Path) -> PerfResult | None:
    """Extract the run parameters from the filename and event counts from perf CSV."""
    match = FILENAME_PATTERN.match(path.name)
    if match is None:
        return None

    parameters = match.groupdict()
    metrics: dict[str, int] = {}
    with path.open(newline="", encoding="utf-8") as perf_file:
        for row in csv.reader(perf_file):
            if len(row) < 4:
                continue
            event = row[2].split(":", maxsplit=1)[0].strip()
            if event not in METRICS:
                continue
            try:
                metrics[event] = int(row[3].strip().replace(",", ""))
            except ValueError:
                # Ignore events that perf did not count (for example, "<not counted>").
                continue

    return PerfResult(
        kernel=parameters["kernel"],
        working_set_size=int(parameters["n"]),
        threads=int(parameters["threads"]),
        stride=int(parameters["stride"]),
        pattern=parameters["pattern"],
        repeat=int(parameters["repeat"]),
        metrics=metrics,
    )


def load_results(results_dir: Path) -> list[PerfResult]:
    results = [
        result
        for path in sorted(results_dir.glob("*.perf.txt"))
        if (result := parse_perf_file(path)) is not None
    ]
    if not results:
        raise SystemExit(f"No matching perf result files found in {results_dir}")
    return results


def median_by_key(results: list[PerfResult], key_fn, metric: str) -> dict:
    values: dict[object, list[int]] = defaultdict(list)
    for result in results:
        if metric in result.metrics:
            values[key_fn(result)].append(result.metrics[metric])
    return {key: statistics.median(counts) for key, counts in values.items()}


def plot_misses_by_working_set(
    results: list[PerfResult], metric: str, output_path: Path
) -> None:
    comparable = [
        result
        for result in results
        if result.threads == 1
        and result.stride == 1
        and result.pattern == "stride"
    ]
    fig, ax = plt.subplots(figsize=(9, 6))

    for kernel in KERNELS:
        medians = median_by_key(
            [result for result in comparable if result.kernel == kernel],
            lambda result: result.working_set_size,
            metric,
        )
        sizes = sorted(size for size, count in medians.items() if count > 0)
        if sizes:
            ax.plot(
                sizes,
                [medians[size] for size in sizes],
                marker="o",
                linewidth=2,
                label=kernel.replace("-", " ").title(),
            )

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Working-set size (elements)")
    ax.set_ylabel("Median misses")
    ax.set_title(f"{metric.replace('-', ' ').title()} vs. Working-Set Size")
    ax.grid(True, which="both", linestyle="--", alpha=0.35)
    ax.legend(title="Kernel")
    fig.tight_layout()
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def access_configurations(results: list[PerfResult]) -> list[tuple[str, int]]:
    configs = {
        (result.pattern, result.stride)
        for result in results
        if result.working_set_size == WORKING_SET_SIZE and result.threads == 1
    }
    return sorted(
        configs,
        key=lambda config: (config[0] == "random", config[1]),
    )


def plot_misses_by_access_pattern(
    results: list[PerfResult], metric: str, output_path: Path
) -> None:
    comparable = [
        result
        for result in results
        if result.working_set_size == WORKING_SET_SIZE and result.threads == 1
    ]
    configurations = access_configurations(comparable)
    if not configurations:
        raise SystemExit(
            f"No single-thread results found for n={WORKING_SET_SIZE:,}"
        )

    medians = median_by_key(
        comparable,
        lambda result: (result.kernel, result.pattern, result.stride),
        metric,
    )
    labels = [
        f"Random" if pattern == "random" else f"Stride {stride}"
        for pattern, stride in configurations
    ]
    x_positions = list(range(len(configurations)))
    bar_width = 0.19
    fig, ax = plt.subplots(figsize=(12, 6.5))

    for kernel_index, kernel in enumerate(KERNELS):
        offset = (kernel_index - (len(KERNELS) - 1) / 2) * bar_width
        values = [
            medians.get((kernel, pattern, stride), 0)
            for pattern, stride in configurations
        ]
        ax.bar(
            [position + offset for position in x_positions],
            values,
            width=bar_width,
            label=kernel.replace("-", " ").title(),
        )

    ax.set_xticks(x_positions, labels)
    ax.set_xlabel("Access pattern")
    ax.set_ylabel("Median misses")
    ax.set_title(
        f"{metric.replace('-', ' ').title()} by Stride / Pattern "
        f"(n={WORKING_SET_SIZE:,} elements)"
    )
    ax.grid(True, axis="y", linestyle="--", alpha=0.35)
    ax.set_axisbelow(True)
    ax.legend(title="Kernel", ncols=2)
    fig.tight_layout()
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    repository_dir = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Generate cache/TLB miss charts from perf result files."
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=repository_dir / "results_small_pages",
        help="Directory containing *.perf.txt files (default: %(default)s)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Directory for chart PNGs (default: %(default)s)",
    )
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    results = load_results(args.results_dir)
    charts = (
        ("cache-misses", "cache_misses_vs_working_set_size.png", plot_misses_by_working_set),
        ("dTLB-load-misses", "tlb_misses_vs_working_set_size.png", plot_misses_by_working_set),
        ("cache-misses", "cache_misses_by_stride_and_pattern.png", plot_misses_by_access_pattern),
        ("dTLB-load-misses", "tlb_misses_by_stride_and_pattern.png", plot_misses_by_access_pattern),
    )
    for metric, filename, plot_function in charts:
        output_path = args.output_dir / filename
        plot_function(results, metric, output_path)
        print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
