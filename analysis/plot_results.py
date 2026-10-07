#!/usr/bin/env python3
"""Plot cache miss and page-walk rates from the benchmark log."""

from __future__ import annotations

import argparse
import re
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt


WORKING_SET_SIZE = 4_194_304
CACHE_METRICS = (
    ("l1_load_miss_rate", "L1 load miss rate"),
    ("l2_miss_rate", "L2 miss rate"),
    ("llc_miss_rate", "LLC miss rate"),
)
PAGE_WALK_METRIC = ("dtlb_page_walk_rate", "DTLB page-walk rate")
FIELD_PATTERN = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)")


@dataclass(frozen=True)
class Result:
    kernel: str
    elements: int
    stride: int
    pattern: str
    threads: int
    metrics: dict[str, float]


def parse_log_line(line: str) -> Result | None:
    """Parse one whitespace-separated key=value record from the benchmark log."""
    fields = dict(FIELD_PATTERN.findall(line))
    required = ("kernel", "elements", "stride", "pattern", "threads")
    if not all(key in fields for key in required):
        return None

    metrics: dict[str, float] = {}
    for key in (*[metric for metric, _ in CACHE_METRICS], PAGE_WALK_METRIC[0]):
        value = fields.get(key)
        if value is None or value == "NA":
            continue
        try:
            metrics[key] = float(value.rstrip("%"))
        except ValueError:
            continue

    return Result(
        kernel=fields["kernel"],
        elements=int(fields["elements"]),
        stride=int(fields["stride"]),
        pattern=fields["pattern"],
        threads=int(fields["threads"]),
        metrics=metrics,
    )


def load_results(log_path: Path) -> list[Result]:
    with log_path.open(encoding="utf-8") as log_file:
        results = [
            result
            for line in log_file
            if (result := parse_log_line(line)) is not None
        ]
    if not results:
        raise SystemExit(f"No valid benchmark records found in {log_path}")
    return results


def median_by_key(results: list[Result], key_fn, metric: str) -> dict:
    values: dict[object, list[float]] = defaultdict(list)
    for result in results:
        if metric in result.metrics:
            values[key_fn(result)].append(result.metrics[metric])
    return {key: statistics.median(samples) for key, samples in values.items()}


def kernels_in(results: list[Result]) -> list[str]:
    return sorted({result.kernel for result in results})


def plot_by_working_set(
    results: list[Result], metrics: tuple[tuple[str, str], ...], output_path: Path
) -> None:
    comparable = [
        result
        for result in results
        if result.threads == 1 and result.stride == 1 and result.pattern == "stride"
    ]
    kernels = kernels_in(comparable)
    fig, axes = plt.subplots(1, len(metrics), figsize=(6 * len(metrics), 5), squeeze=False)

    for ax, (metric, title) in zip(axes[0], metrics):
        for kernel in kernels:
            selected = [result for result in comparable if result.kernel == kernel]
            medians = median_by_key(selected, lambda result: result.elements, metric)
            sizes = sorted(medians)
            if sizes:
                ax.plot(sizes, [medians[size] for size in sizes], marker="o", label=kernel)
        ax.set_xscale("log")
        ax.set_xlabel("Working-set size (elements)")
        ax.set_ylabel("Miss rate (%)")
        ax.set_title(title)
        ax.grid(True, which="both", linestyle="--", alpha=0.35)
        ax.legend(fontsize="small")

    fig.suptitle("Cache Miss Rate vs. Working-Set Size")
    fig.tight_layout()
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def plot_by_access_pattern(
    results: list[Result], metrics: tuple[tuple[str, str], ...], output_path: Path
) -> None:
    comparable = [
        result
        for result in results
        if result.elements == WORKING_SET_SIZE and result.threads == 1
    ]
    configurations = sorted(
        {(result.pattern, result.stride) for result in comparable},
        key=lambda item: (item[0] == "random", item[1]),
    )
    if not configurations:
        raise SystemExit(f"No single-thread results found for n={WORKING_SET_SIZE:,}")

    kernels = kernels_in(comparable)
    fig, axes = plt.subplots(1, len(metrics), figsize=(6 * len(metrics), 5), squeeze=False)
    positions = list(range(len(configurations)))
    labels = ["Random" if pattern == "random" else f"Stride {stride}" for pattern, stride in configurations]

    for ax, (metric, title) in zip(axes[0], metrics):
        medians = median_by_key(
            comparable,
            lambda result: (result.kernel, result.pattern, result.stride),
            metric,
        )
        bar_width = 0.8 / max(len(kernels), 1)
        for kernel_index, kernel in enumerate(kernels):
            offset = (kernel_index - (len(kernels) - 1) / 2) * bar_width
            values = [
                medians.get((kernel, pattern, stride), 0.0)
                for pattern, stride in configurations
            ]
            ax.bar(
                [position + offset for position in positions],
                values,
                width=bar_width,
                label=kernel,
            )
        ax.set_xticks(positions, labels, rotation=35, ha="right")
        ax.set_xlabel("Stride / access pattern")
        ax.set_ylabel("Rate (%)")
        ax.set_title(title)
        ax.grid(True, axis="y", linestyle="--", alpha=0.35)
        ax.legend(fontsize="small")

    fig.suptitle(f"Cache Miss Rate by Access Pattern (n={WORKING_SET_SIZE:,})")
    fig.tight_layout()
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    repository_dir = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Generate cache miss-rate and page-walk-rate charts."
    )
    parser.add_argument(
        "--results-file",
        type=Path,
        default=repository_dir / "results" / "small-pages.log",
        help="Benchmark log file (default: %(default)s)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Directory for chart PNGs (default: %(default)s)",
    )
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = load_results(args.results_file)

    charts = (
        ("cache_miss_rate_vs_working_set_size.png", CACHE_METRICS, plot_by_working_set),
        ("cache_miss_rate_by_access_pattern.png", CACHE_METRICS, plot_by_access_pattern),
        ("page_walk_rate_vs_working_set_size.png", (PAGE_WALK_METRIC,), plot_by_working_set),
        ("page_walk_rate_by_access_pattern.png", (PAGE_WALK_METRIC,), plot_by_access_pattern),
    )
    for filename, metrics, plot_function in charts:
        output_path = args.output_dir / filename
        plot_function(results, metrics, output_path)
        print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
