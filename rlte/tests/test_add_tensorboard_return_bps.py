from tensorboard.backend.event_processing import event_accumulator
from torch.utils.tensorboard import SummaryWriter

from rl_files.add_tensorboard_return_bps import (
    SOURCE_TAG,
    TARGET_TAG,
    add_return_bps,
    find_run_directories,
)


def scalar_values(run_directory, tag):
    accumulator = event_accumulator.EventAccumulator(
        str(run_directory),
        size_guidance={event_accumulator.SCALARS: 0},
    )
    accumulator.Reload()
    return accumulator.Scalars(tag)


def test_add_return_bps_preserves_steps_and_multiplies_values(tmp_path):
    run_directory = tmp_path / "run"
    writer = SummaryWriter(str(run_directory))
    writer.add_scalar(SOURCE_TAG, -0.5, global_step=400, walltime=10.0)
    writer.add_scalar(SOURCE_TAG, 1.25, global_step=800, walltime=20.0)
    writer.close()

    assert find_run_directories(tmp_path) == [run_directory]
    assert add_return_bps(run_directory, multiplier=10.0) == 2

    derived_events = scalar_values(run_directory, TARGET_TAG)
    assert [event.step for event in derived_events] == [400, 800]
    assert [event.value for event in derived_events] == [-5.0, 12.5]
    assert [event.wall_time for event in derived_events] == [10.0, 20.0]

    # A second invocation must not append duplicate points.
    assert add_return_bps(run_directory, multiplier=10.0) == 0
    assert len(scalar_values(run_directory, TARGET_TAG)) == 2
