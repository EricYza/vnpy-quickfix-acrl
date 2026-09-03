"""Add a basis-point return series to existing TensorBoard runs.

The simulator currently starts around an initial best bid of 1000 and uses a
price tick of 1, so its return in simulated ticks converts to basis points as

    return_bps = return * 1 / 1000 * 10_000 = return * 10.

This utility reads ``charts/return`` from existing event files and writes the
derived values back to each run under ``charts/return_bps``.  Training does not
need to be repeated.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from tensorboard.backend.event_processing import event_accumulator
from torch.utils.tensorboard import SummaryWriter


SOURCE_TAG = "charts/return"
TARGET_TAG = "charts/return_bps"
EVENT_PATTERN = "events.out.tfevents.*"


def find_run_directories(log_path: Path) -> list[Path]:
    """Return event-file directories below one run or a log root."""

    if not log_path.exists():
        raise FileNotFoundError(f"TensorBoard path does not exist: {log_path}")

    event_files = list(log_path.glob(EVENT_PATTERN))
    if event_files:
        return [log_path]

    return sorted(
        {event_file.parent for event_file in log_path.rglob(EVENT_PATTERN)}
    )


def add_return_bps(run_directory: Path, multiplier: float) -> int:
    """Write the derived series and return the number of points added."""

    accumulator = event_accumulator.EventAccumulator(
        str(run_directory),
        size_guidance={event_accumulator.SCALARS: 0},
    )
    accumulator.Reload()
    scalar_tags = set(accumulator.Tags().get("scalars", []))

    if SOURCE_TAG not in scalar_tags:
        print(f"skip (no {SOURCE_TAG}): {run_directory}")
        return 0
    if TARGET_TAG in scalar_tags:
        print(f"skip (already has {TARGET_TAG}): {run_directory}")
        return 0

    return_events = accumulator.Scalars(SOURCE_TAG)
    writer = SummaryWriter(
        log_dir=str(run_directory),
        filename_suffix=".return_bps",
    )
    try:
        for scalar_event in return_events:
            writer.add_scalar(
                TARGET_TAG,
                scalar_event.value * multiplier,
                global_step=scalar_event.step,
                walltime=scalar_event.wall_time,
            )
    finally:
        writer.close()

    print(
        f"added {len(return_events)} points to {run_directory}: "
        f"{TARGET_TAG} = {SOURCE_TAG} * {multiplier:g}"
    )
    return len(return_events)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Backfill charts/return_bps into TensorBoard event logs."
    )
    parser.add_argument(
        "log_path",
        type=Path,
        help="One TensorBoard run directory or a root containing multiple runs.",
    )
    parser.add_argument(
        "--multiplier",
        type=float,
        default=10.0,
        help="Conversion multiplier from logged return to bps (default: 10).",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not math.isfinite(args.multiplier) or args.multiplier <= 0:
        raise ValueError("multiplier must be a finite positive number")

    run_directories = find_run_directories(args.log_path.resolve())
    if not run_directories:
        raise FileNotFoundError(
            f"no TensorBoard event files found below: {args.log_path}"
        )

    total_points = sum(
        add_return_bps(run_directory, args.multiplier)
        for run_directory in run_directories
    )
    print(
        f"finished: checked {len(run_directories)} run(s), "
        f"added {total_points} point(s)"
    )


if __name__ == "__main__":
    main()
