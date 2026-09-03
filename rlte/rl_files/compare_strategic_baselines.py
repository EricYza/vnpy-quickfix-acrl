"""Compare the trained strategic PPO policy with execution baselines.

This file is intentionally configured from the ``CONFIG`` block below so it
can be opened and run directly from an IDE.  No command-line arguments are
required.

The comparison uses one market seed per episode.  Every policy receives the
same ordered list of seeds, making the reward differences paired by seed.

Baselines
---------
PPO
    Deterministic action from the trained logistic-normal PPO policy.
Market-TWAP
    Sells an equal slice with a market order at every decision time.
SL
    Submits the full inventory at the initial best ask and leaves it there;
    any remainder is sold at the deadline.
Linear-SL
    Submits an equal limit-order slice at every decision time and leaves all
    previous slices in the book; any remainder is sold at the deadline.  This
    is a passive, limit-order TWAP-like baseline rather than strict TWAP.
Immediate-Market
    Sells the full inventory immediately with a market order.
"""

from __future__ import annotations

import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import gymnasium as gym
import numpy as np
import torch


CURRENT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = CURRENT_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from rl_files.ppo_logistic_normal import AgentLogisticNormal
from simulation.market_gym import Market


@dataclass(frozen=True)
class ComparisonConfig:
    """Settings for one comparison run."""

    model_path: Path
    output_directory: Path

    # Start with 200 as a quick check.  Change this to 1000 for the formal run.
    episodes: int = 200
    first_seed: int = 100
    vector_batch_size: int = 20
    use_cuda: bool = True

    market_environment: str = "strategic"
    volume: int = 20
    terminal_time: int = 150
    time_delta: int = 15
    drop_feature: str | None = None


# ---------------------------------------------------------------------------
# EDIT THIS BLOCK, THEN RUN THIS FILE DIRECTLY.
# ---------------------------------------------------------------------------
CONFIG = ComparisonConfig(
    model_path=(
        PROJECT_ROOT
        / "models"
        / (
            "strategic_20_seed_0_eval_seed_100_eval_episodes_1000_"
            "num_iterations_500_bsize_400_"
            "ppo_logistic_normal_comparison_200k.pt"
        )
    ),
    output_directory=PROJECT_ROOT / "rewards",
    episodes=200,
    first_seed=100,
    vector_batch_size=20,
    use_cuda=True,
    market_environment="strategic",
    volume=20,
    terminal_time=150,
    time_delta=15,
    drop_feature=None,
)


MetricArrays = dict[str, np.ndarray]


def validate_config(config: ComparisonConfig) -> None:
    if not config.model_path.is_file():
        raise FileNotFoundError(f"model not found: {config.model_path}")
    if config.episodes < 2:
        raise ValueError("episodes must be at least 2")
    if config.vector_batch_size < 1:
        raise ValueError("vector_batch_size must be positive")
    if config.market_environment not in {"noise", "flow", "strategic"}:
        raise ValueError(
            f"unknown market environment: {config.market_environment}"
        )
    if config.volume <= 0:
        raise ValueError("volume must be positive")
    if config.terminal_time <= 0 or config.time_delta <= 0:
        raise ValueError("terminal_time and time_delta must be positive")
    if config.terminal_time % config.time_delta != 0:
        raise ValueError(
            "this comparison requires terminal_time to be divisible by "
            "time_delta"
        )

    decision_count = config.terminal_time // config.time_delta
    if config.volume % decision_count != 0:
        raise ValueError(
            "volume must be divisible by the number of decision intervals "
            "for an equal-slice Market-TWAP comparison"
        )


def make_market_config(
    config: ComparisonConfig,
    seed: int,
    execution_agent: str,
) -> dict:
    return {
        "seed": int(seed),
        "market_env": config.market_environment,
        "execution_agent": execution_agent,
        "volume": config.volume,
        "terminal_time": config.terminal_time,
        "time_delta": config.time_delta,
        "drop_feature": config.drop_feature,
    }


def make_vector_environment(
    config: ComparisonConfig,
    seeds: np.ndarray,
    execution_agent: str,
) -> gym.vector.SyncVectorEnv:
    environment_functions = []
    for seed in seeds:
        market_config = make_market_config(
            config,
            int(seed),
            execution_agent,
        )
        environment_functions.append(
            lambda market_config=market_config: Market(market_config)
        )
    return gym.vector.SyncVectorEnv(environment_functions)


def load_policy(
    config: ComparisonConfig,
    device: torch.device,
) -> AgentLogisticNormal:
    probe_environment = make_vector_environment(
        config,
        np.asarray([config.first_seed]),
        "rl_agent",
    )
    try:
        policy = AgentLogisticNormal(probe_environment).to(device)
        try:
            state_dict = torch.load(
                config.model_path,
                map_location=device,
                weights_only=True,
            )
        except TypeError:
            # Compatibility with PyTorch versions that predate weights_only.
            state_dict = torch.load(config.model_path, map_location=device)
        policy.load_state_dict(state_dict)
        policy.eval()
        return policy
    finally:
        probe_environment.close()


def empty_batch_metrics(size: int) -> MetricArrays:
    return {
        "rewards": np.full(size, np.nan, dtype=float),
        "times": np.full(size, np.nan, dtype=float),
        "passive_fill_rates": np.full(size, np.nan, dtype=float),
        "forced_liquidation_rates": np.full(size, np.nan, dtype=float),
        "drifts": np.full(size, np.nan, dtype=float),
    }


def record_final_information(
    infos: dict,
    completed: np.ndarray,
    metrics: MetricArrays,
) -> None:
    final_infos = infos.get("final_info")
    if final_infos is None:
        return

    for index, info in enumerate(final_infos):
        if completed[index] or info is None:
            continue
        metrics["rewards"][index] = info["reward"]
        metrics["times"][index] = info["time"]
        metrics["passive_fill_rates"][index] = info["passive_fill_rate"]
        metrics["forced_liquidation_rates"][index] = info[
            "forced_liquidation_rate"
        ]
        metrics["drifts"][index] = info["drift"]
        completed[index] = True


def evaluate_dynamic_policy(
    config: ComparisonConfig,
    seeds: np.ndarray,
    label: str,
    action_function: Callable[[np.ndarray], np.ndarray],
) -> MetricArrays:
    collected: dict[str, list[float]] = {
        "rewards": [],
        "times": [],
        "passive_fill_rates": [],
        "forced_liquidation_rates": [],
        "drifts": [],
    }
    maximum_steps = config.terminal_time // config.time_delta + 2

    for start in range(0, len(seeds), config.vector_batch_size):
        batch_seeds = seeds[start : start + config.vector_batch_size]
        environments = make_vector_environment(
            config,
            batch_seeds,
            "rl_agent",
        )
        try:
            observations, _ = environments.reset()
            completed = np.zeros(len(batch_seeds), dtype=bool)
            batch_metrics = empty_batch_metrics(len(batch_seeds))

            for _ in range(maximum_steps):
                actions = np.asarray(action_function(observations))
                expected_shape = (
                    len(batch_seeds),
                    environments.single_action_space.shape[0],
                )
                if actions.shape != expected_shape:
                    raise ValueError(
                        f"{label} produced actions with shape {actions.shape}; "
                        f"expected {expected_shape}"
                    )
                if not np.isfinite(actions).all():
                    raise FloatingPointError(
                        f"{label} produced a non-finite action"
                    )

                observations, _, _, _, infos = environments.step(actions)
                record_final_information(infos, completed, batch_metrics)
                if completed.all():
                    break
        finally:
            environments.close()

        if not completed.all():
            missing_seeds = batch_seeds[~completed]
            raise RuntimeError(
                f"{label} did not finish seeds: {missing_seeds.tolist()}"
            )

        for metric_name in collected:
            collected[metric_name].extend(batch_metrics[metric_name].tolist())

        progress = min(start + len(batch_seeds), len(seeds))
        print(f"{label}: {progress}/{len(seeds)}")

    return {
        name: np.asarray(values, dtype=float)
        for name, values in collected.items()
    }


def evaluate_existing_baseline(
    config: ComparisonConfig,
    seeds: np.ndarray,
    execution_agent: str,
    label: str,
) -> MetricArrays:
    collected: dict[str, list[float]] = {
        "rewards": [],
        "times": [],
        "passive_fill_rates": [],
        "forced_liquidation_rates": [],
        "drifts": [],
    }

    for start in range(0, len(seeds), config.vector_batch_size):
        batch_seeds = seeds[start : start + config.vector_batch_size]
        environments = make_vector_environment(
            config,
            batch_seeds,
            execution_agent,
        )
        try:
            _, infos = environments.reset()
        finally:
            environments.close()

        required_fields = {
            "reward",
            "time",
            "passive_fill_rate",
            "forced_liquidation_rate",
            "drift",
        }
        missing_fields = required_fields.difference(infos)
        if missing_fields:
            raise KeyError(
                f"{label} result is missing fields: {sorted(missing_fields)}"
            )

        collected["rewards"].extend(np.asarray(infos["reward"]).tolist())
        collected["times"].extend(np.asarray(infos["time"]).tolist())
        collected["passive_fill_rates"].extend(
            np.asarray(infos["passive_fill_rate"]).tolist()
        )
        collected["forced_liquidation_rates"].extend(
            np.asarray(infos["forced_liquidation_rate"]).tolist()
        )
        collected["drifts"].extend(np.asarray(infos["drift"]).tolist())

        progress = min(start + len(batch_seeds), len(seeds))
        print(f"{label}: {progress}/{len(seeds)}")

    return {
        name: np.asarray(values, dtype=float)
        for name, values in collected.items()
    }


def summary_row(
    name: str,
    metrics: MetricArrays,
    terminal_time: int,
) -> dict[str, float | int | str | bool]:
    rewards = metrics["rewards"]
    times = metrics["times"]
    passive_fill_rates = metrics["passive_fill_rates"]
    forced_liquidation_rates = metrics["forced_liquidation_rates"]
    percentiles = np.percentile(rewards, [5, 50, 95])
    return {
        "policy": name,
        "episodes": len(rewards),
        "all_finite": bool(np.isfinite(rewards).all()),
        "mean_reward": float(rewards.mean()),
        "std_reward": float(rewards.std()),
        "reward_p05": float(percentiles[0]),
        "reward_p50": float(percentiles[1]),
        "reward_p95": float(percentiles[2]),
        "positive_rate": float(np.mean(rewards > 0)),
        "mean_finish_time": float(times.mean()),
        "early_completion_rate": float(np.mean(times < terminal_time)),
        "mean_passive_fill_rate": float(passive_fill_rates.mean()),
        "mean_forced_liquidation_rate": float(
            forced_liquidation_rates.mean()
        ),
    }


def paired_row(
    ppo_rewards: np.ndarray,
    baseline_name: str,
    baseline_rewards: np.ndarray,
) -> dict[str, float | str]:
    differences = ppo_rewards - baseline_rewards
    standard_error = differences.std(ddof=1) / np.sqrt(len(differences))
    percentiles = np.percentile(differences, [5, 50, 95])
    tolerance = 1e-12
    return {
        "comparison": f"ppo_minus_{baseline_name}",
        "mean_difference": float(differences.mean()),
        "ci95_low": float(differences.mean() - 1.96 * standard_error),
        "ci95_high": float(differences.mean() + 1.96 * standard_error),
        "difference_p05": float(percentiles[0]),
        "difference_p50": float(percentiles[1]),
        "difference_p95": float(percentiles[2]),
        "ppo_win_rate": float(np.mean(differences > tolerance)),
        "tie_rate": float(np.mean(np.abs(differences) <= tolerance)),
        "ppo_loss_rate": float(np.mean(differences < -tolerance)),
    }


def print_rows(title: str, rows: list[dict]) -> None:
    print(f"\n{'=' * 10} {title} {'=' * 10}")
    for row in rows:
        print()
        for name, value in row.items():
            if isinstance(value, float):
                print(f"{name}: {value:.6f}")
            else:
                print(f"{name}: {value}")


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def run_comparison(config: ComparisonConfig = CONFIG) -> dict[str, MetricArrays]:
    validate_config(config)
    config.output_directory.mkdir(parents=True, exist_ok=True)

    seeds = np.arange(
        config.first_seed,
        config.first_seed + config.episodes,
        dtype=int,
    )
    device = torch.device(
        "cuda"
        if config.use_cuda and torch.cuda.is_available()
        else "cpu"
    )
    decision_count = config.terminal_time // config.time_delta
    market_twap_slice = config.volume // decision_count

    print("Starting paired strategic-policy comparison")
    print(f"device: {device}")
    print(f"model: {config.model_path}")
    print(f"episodes per policy: {config.episodes}")
    print(f"seeds: {seeds[0]}..{seeds[-1]}")
    print(
        "task: "
        f"environment={config.market_environment}, "
        f"volume={config.volume}, "
        f"terminal_time={config.terminal_time}, "
        f"time_delta={config.time_delta}"
    )
    print(f"Market-TWAP slice per decision: {market_twap_slice}")

    policy = load_policy(config, device)

    def ppo_action(observations: np.ndarray) -> np.ndarray:
        with torch.no_grad():
            actions = policy.deterministic_action(
                torch.as_tensor(
                    observations,
                    dtype=torch.float32,
                    device=device,
                )
            )
        return actions.cpu().numpy()

    def market_twap_action(observations: np.ndarray) -> np.ndarray:
        # Observation index 1 is remaining inventory / initial inventory when
        # drop_feature=None (and remains index 1 for the supported drop modes).
        remaining_inventory = np.rint(
            observations[:, 1] * config.volume
        ).astype(int)
        actions = np.zeros((len(observations), 7), dtype=np.float32)
        for index, inventory in enumerate(remaining_inventory):
            if inventory <= 0:
                actions[index, -1] = 1.0
                continue
            quantity = min(market_twap_slice, inventory)
            actions[index, 0] = quantity / inventory
            actions[index, -1] = 1.0 - actions[index, 0]
        return actions

    def immediate_market_action(observations: np.ndarray) -> np.ndarray:
        actions = np.zeros((len(observations), 7), dtype=np.float32)
        actions[:, 0] = 1.0
        return actions

    results = {
        "ppo": evaluate_dynamic_policy(
            config,
            seeds,
            "PPO",
            ppo_action,
        ),
        "market_twap": evaluate_dynamic_policy(
            config,
            seeds,
            "Market-TWAP",
            market_twap_action,
        ),
        "sl": evaluate_existing_baseline(
            config,
            seeds,
            "sl_agent",
            "SL",
        ),
        "linear_sl": evaluate_existing_baseline(
            config,
            seeds,
            "linear_sl_agent",
            "Linear-SL",
        ),
        # Use the RL execution path with a fixed 100% market action.  The
        # project's legacy MarketAgent lacks the terminal_time attribute that
        # Market.transition expects, whereas this produces the same execution
        # intent without depending on that unrelated legacy defect.
        "immediate_market": evaluate_dynamic_policy(
            config,
            seeds,
            "Immediate-Market",
            immediate_market_action,
        ),
    }

    summary_rows = [
        summary_row(name, metrics, config.terminal_time)
        for name, metrics in results.items()
    ]
    paired_rows = [
        paired_row(
            results["ppo"]["rewards"],
            baseline_name,
            results[baseline_name]["rewards"],
        )
        for baseline_name in (
            "market_twap",
            "sl",
            "linear_sl",
            "immediate_market",
        )
    ]

    print_rows("POLICY SUMMARIES", summary_rows)
    print_rows("PAIRED PPO DIFFERENCES", paired_rows)

    output_stem = (
        f"{config.market_environment}_{config.volume}_ppo_baselines_"
        f"seed_{config.first_seed}_episodes_{config.episodes}"
    )
    arrays_path = config.output_directory / f"{output_stem}.npz"
    summaries_path = config.output_directory / f"{output_stem}_summary.csv"
    paired_path = config.output_directory / f"{output_stem}_paired.csv"

    saved_arrays: dict[str, np.ndarray] = {"seeds": seeds}
    for policy_name, metrics in results.items():
        for metric_name, values in metrics.items():
            saved_arrays[f"{policy_name}_{metric_name}"] = values
    np.savez(arrays_path, **saved_arrays)
    write_csv(summaries_path, summary_rows)
    write_csv(paired_path, paired_rows)

    print("\nSaved files:")
    print(arrays_path)
    print(summaries_path)
    print(paired_path)
    return results


if __name__ == "__main__":
    run_comparison()
