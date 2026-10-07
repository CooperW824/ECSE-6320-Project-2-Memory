#!/usr/bin/env python3
"""Plot latency and bandwidth from Intel MLC loaded-latency output files."""

from __future__ import annotations

import argparse
import re
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt


NUMBER = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
ROW_PATTERN = re.compile(rf"^\s*({NUMBER})\s+({NUMBER})\s+({NUMBER})\s*$")

ACCESS_LABELS = {
    "stride-64b": "64 B stride",
    "stride-256b": "256 B stride",
    "stride-512b": "512 B stride",
    "stride-1024b": "1024 B stride",
    "random": "Random access",
}
ACCESS_ORDER = ("stride-64b", "stride-256b", "stride-512b", "stride-1024b", "random")
MIX_LABELS = {
    "mix-100read": "100% read",
    "mix-66read-33write": "66% read / 33% write",
    "mix-50read-50write": "50% read / 50% write",
    "mix-100write": "100% write",
}
MIX_ORDER = tuple(MIX_LABELS)


@dataclass(frozen=True)
class Measurement:
    injection_delay: float
    latency_ns: float
    bandwidth_mb_s: float


def parse_output(path: Path) -> list[Measurement]:
    """Read the numeric injection-delay, latency, bandwidth rows from an MLC file."""
    measurements: list[Measurement] = []
    with path.open(encoding="utf-8", errors="replace") as output:
        for line in output:
            match = ROW_PATTERN.fullmatch(line)
            if match:
                delay, latency, bandwidth = (float(value) for value in match.groups())
                measurements.append(Measurement(delay, latency, bandwidth))
    if not measurements:
        raise ValueError(f"No loaded-latency data rows found in {path}")
    return sorted(measurements, key=lambda point: point.injection_delay)


def plot_latency_bandwidth(
    datasets: list[tuple[str, list[Measurement]]], title: str, output_path: Path
) -> None:
    fig, (latency_ax, bandwidth_ax) = plt.subplots(
        2, 1, figsize=(9, 8), sharex=True, constrained_layout=True
    )
    for label, points in datasets:
        delays = [point.injection_delay for point in points]
        latency_ax.plot(
            delays,
            [point.latency_ns for point in points],
            marker="o",
            markersize=3.5,
            label=label,
        )
        bandwidth_ax.plot(
            delays,
            [point.bandwidth_mb_s for point in points],
            marker="o",
            markersize=3.5,
            label=label,
        )

    latency_ax.set_ylabel("Latency (ns)")
    latency_ax.set_title("Latency")
    bandwidth_ax.set_ylabel("Bandwidth (MB/s)")
    bandwidth_ax.set_xlabel("MLC injection delay (ns)")
    bandwidth_ax.set_title("Bandwidth")
    for ax in (latency_ax, bandwidth_ax):
        ax.grid(True, linestyle="--", alpha=0.35)
        ax.legend(fontsize="small", ncols=2)
    fig.suptitle(title)
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def configuration_from_path(path: Path) -> str | None:
    """Return the recognized loaded-latency configuration encoded in a filename."""
    stem = path.stem
    prefix = "loaded-latency-"
    if not stem.startswith(prefix):
        return None
    configuration = stem[len(prefix) :]
    if configuration in ACCESS_LABELS or configuration in MIX_LABELS:
        return configuration
    return None


def main() -> None:
    repository_dir = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Generate plots from Intel MLC loaded-latency result files."
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=repository_dir / "results" / "mlc",
        help="MLC result root or one run directory (default: %(default)s)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Directory for chart PNGs (default: %(default)s)",
    )
    args = parser.parse_args()

    if not args.results_dir.is_dir():
        parser.error(f"results directory does not exist: {args.results_dir}")

    by_run: dict[Path, dict[str, list[Measurement]]] = defaultdict(dict)
    bad_files: list[str] = []
    for path in sorted(args.results_dir.rglob("loaded-latency-*.txt")):
        configuration = configuration_from_path(path)
        if configuration is None:
            continue
        try:
            by_run[path.parent][configuration] = parse_output(path)
        except ValueError as error:
            bad_files.append(str(error))

    if not by_run:
        details = "\n".join(bad_files)
        message = f"No usable loaded-latency files found under {args.results_dir}"
        if details:
            message += f"\n{details}"
        parser.error(message)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    multiple_runs = len(by_run) > 1
    for run_dir, configurations in sorted(by_run.items()):
        suffix = f"_{run_dir.name}" if multiple_runs else ""
        access_data = [
            (ACCESS_LABELS[key], configurations[key])
            for key in ACCESS_ORDER
            if key in configurations
        ]
        mix_data = [
            (MIX_LABELS[key], configurations[key])
            for key in MIX_ORDER
            if key in configurations
        ]

        if access_data:
            output_path = args.output_dir / f"mlc_access_pattern_latency_bandwidth{suffix}.png"
            plot_latency_bandwidth(
                access_data, "MLC Latency and Bandwidth by Access Pattern", output_path
            )
            print(f"Wrote {output_path}")
        if mix_data:
            output_path = args.output_dir / f"mlc_read_write_mix_latency_bandwidth{suffix}.png"
            plot_latency_bandwidth(
                mix_data, "MLC Latency and Bandwidth by Read/Write Mix", output_path
            )
            print(f"Wrote {output_path}")
        if "mix-100read" in configurations:
            output_path = args.output_dir / f"mlc_100_read_latency_bandwidth{suffix}.png"
            plot_latency_bandwidth(
                [(MIX_LABELS["mix-100read"], configurations["mix-100read"])],
                "MLC Latency and Bandwidth — 100% Read Mix",
                output_path,
            )
            print(f"Wrote {output_path}")

    for message in bad_files:
        print(f"Skipped: {message}")


if __name__ == "__main__":
    main()
