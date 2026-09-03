"""PPO trainer for the market environment's logistic-normal policy.

This module intentionally keeps the observation pipeline, policy architecture,
environment construction, model format, and deterministic evaluation used by
``actor_critic.py``.  The policy update is replaced by PPO's clipped surrogate
objective and supports repeated minibatch updates, KL diagnostics, and gradient
clipping.
"""

from __future__ import annotations

import random
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import gymnasium as gym
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
import tyro
from torch.distributions.normal import Normal
from torch.utils.tensorboard import SummaryWriter


CURRENT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = CURRENT_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.append(str(PROJECT_ROOT))

from simulation.market_gym import Market


@dataclass
class Args:
    # Experiment and output settings.
    tag: Optional[str] = None
    """Additional label appended to the generated run name."""
    run_name: Optional[str] = None
    """Explicit run name. When omitted, a descriptive name is generated."""
    output_dir: str = str(PROJECT_ROOT)
    """Root directory containing tensorboard_logs, models, and rewards."""
    seed: int = 0
    """Training seed."""
    eval_seed: int = 100
    """First evaluation seed."""
    torch_deterministic: bool = True
    """Use deterministic PyTorch kernels where available."""
    cuda: bool = True
    """Use CUDA when it is available."""
    save_model: bool = True
    """Save the final model before evaluation."""
    evaluate: bool = True
    """Evaluate the final deterministic policy."""
    n_eval_episodes: int = 1000
    """Number of final evaluation episodes."""
    eval_save_interval: int = 100
    """Save partial evaluation results after this many episodes; zero disables it."""

    # Market settings.
    env_type: str = "strategic"
    """Market regime: noise, flow, or strategic."""
    num_lots: int = 20
    """Initial execution inventory."""
    terminal_time: int = 150
    """Execution deadline."""
    time_delta: int = 15
    """Time between policy decisions."""
    drop_feature: Optional[str] = None
    """Optional observation feature group to drop."""

    # Sampling and optimization settings.
    total_timesteps: int = 200_000
    learning_rate: float = 5e-4
    num_envs: int = 4
    num_steps: int = 100
    anneal_lr: bool = False
    gamma: float = 1.0
    gae_lambda: float = 1.0
    num_minibatches: int = 4
    update_epochs: int = 4
    norm_adv: bool = True
    clip_coef: float = 0.2
    clip_vloss: bool = False
    ent_coef: float = 0.0
    vf_coef: float = 0.5
    max_grad_norm: float = 0.5
    target_kl: Optional[float] = 0.02

    # Computed at runtime.
    batch_size: int = 0
    minibatch_size: int = 0
    num_iterations: int = 0


def make_env(config):
    def thunk():
        return Market(config)

    return thunk


def layer_init(layer, std=np.sqrt(2), bias_const=0.0):
    torch.nn.init.orthogonal_(layer.weight, std)
    torch.nn.init.constant_(layer.bias, bias_const)
    return layer


class AgentLogisticNormal(nn.Module):
    """Actor-critic model kept compatible with ``actor_critic.py``."""

    def __init__(self, envs, variance_scaling=True):
        super().__init__()
        n_hidden_units = 128
        observation_size = int(np.array(envs.single_observation_space.shape).prod())
        action_size = int(np.prod(envs.single_action_space.shape))

        self.critic = nn.Sequential(
            layer_init(nn.Linear(observation_size, n_hidden_units)),
            nn.Tanh(),
            layer_init(nn.Linear(n_hidden_units, n_hidden_units)),
            nn.Tanh(),
            layer_init(nn.Linear(n_hidden_units, 1), std=1.0),
        )
        self.actor_mean = nn.Sequential(
            layer_init(nn.Linear(observation_size, n_hidden_units)),
            nn.Tanh(),
            layer_init(nn.Linear(n_hidden_units, n_hidden_units)),
            nn.Tanh(),
            layer_init(nn.Linear(n_hidden_units, action_size - 1), std=1e-5),
        )

        initial_bias = -torch.ones(action_size - 1)
        initial_bias[-1] = 1.0
        self.actor_mean[-1].bias.data.copy_(initial_bias)

        self.variance_scaling = variance_scaling
        if variance_scaling:
            self.variance = 1.0
        else:
            self.variance = None
            self.log_std = nn.Parameter(torch.zeros(action_size - 1), requires_grad=True)

    def get_value(self, observation):
        return self.critic(observation)

    def get_action_and_value(self, observation, action=None):
        action_mean = self.actor_mean(observation)
        if self.variance_scaling:
            action_std = torch.ones_like(action_mean) * self.variance
        else:
            action_std = torch.exp(self.log_std.expand_as(action_mean))
        distribution = Normal(action_mean, action_std)

        # The action transformation is fixed and bijective. Its Jacobian is the
        # same for the old and new policies and therefore cancels in PPO's
        # probability ratio. The base Normal entropy remains a diagnostic rather
        # than the exact entropy of the transformed simplex distribution.
        with torch.no_grad():
            if action is None:
                base_action = distribution.sample()
                denominator = 1 + torch.sum(
                    torch.exp(base_action), dim=1, keepdim=True
                )
                action = torch.exp(base_action) / denominator
                action = torch.cat((action, 1 / denominator), dim=1)
            else:
                last_component = action[:, -1].reshape(-1, 1)
                base_action = torch.log(action[:, :-1] / last_component)

        log_probability = distribution.log_prob(base_action).sum(1)
        entropy = distribution.entropy().sum(1)
        return action, log_probability, entropy, self.critic(observation)

    def deterministic_action(self, observation):
        base_action = self.actor_mean(observation)
        with torch.no_grad():
            denominator = 1 + torch.sum(
                torch.exp(base_action), dim=1, keepdim=True
            )
            action = torch.exp(base_action) / denominator
            return torch.cat((action, 1 / denominator), dim=1)


def compute_ppo_policy_loss(
    new_log_probability,
    old_log_probability,
    advantages,
    clip_coef,
):
    """Return PPO's clipped policy loss and its main diagnostics."""

    log_ratio = new_log_probability - old_log_probability
    ratio = log_ratio.exp()
    if not torch.isfinite(ratio).all():
        raise FloatingPointError("non-finite PPO probability ratio")

    loss_unclipped = -advantages * ratio
    loss_clipped = -advantages * torch.clamp(
        ratio,
        1.0 - clip_coef,
        1.0 + clip_coef,
    )
    policy_loss = torch.maximum(loss_unclipped, loss_clipped).mean()

    with torch.no_grad():
        old_approx_kl = (-log_ratio).mean()
        approx_kl = ((ratio - 1.0) - log_ratio).mean()
        clip_fraction = (
            (ratio - 1.0).abs() > clip_coef
        ).float().mean()

    return (
        policy_loss,
        ratio,
        old_approx_kl,
        approx_kl,
        clip_fraction,
    )


def build_configs(args, evaluation=False):
    first_seed = args.eval_seed if evaluation else args.seed
    return [
        {
            "market_env": args.env_type,
            "execution_agent": "rl_agent",
            "volume": args.num_lots,
            "seed": first_seed + worker,
            "terminal_time": args.terminal_time,
            "time_delta": args.time_delta,
            "drop_feature": args.drop_feature,
        }
        for worker in range(args.num_envs)
    ]


def save_evaluation(path, rewards, times, drifts, passive_fill_rates):
    np.savez(
        path,
        rewards=np.asarray(rewards, dtype=float),
        times=np.asarray(times, dtype=float),
        drifts=np.asarray(drifts, dtype=float),
        passive_fill_rates=np.asarray(passive_fill_rates, dtype=float),
    )


def evaluate_final_policy(agent, args, device, reward_path, partial_path):
    """Evaluate one fixed deterministic policy and save the episode arrays."""

    envs = gym.vector.AsyncVectorEnv(
        [make_env(config) for config in build_configs(args, evaluation=True)]
    )
    observations, _ = envs.reset()
    rewards = []
    times = []
    drifts = []
    passive_fill_rates = []
    next_save = args.eval_save_interval

    print("starting deterministic final evaluation")
    try:
        while len(rewards) < args.n_eval_episodes:
            with torch.no_grad():
                actions = agent.deterministic_action(
                    torch.as_tensor(
                        observations,
                        dtype=torch.float32,
                        device=device,
                    )
                )
            observations, _, _, _, infos = envs.step(actions.cpu().numpy())

            if "final_info" in infos:
                for info in infos["final_info"]:
                    if info is None:
                        continue
                    rewards.append(info["reward"])
                    times.append(info["time"])
                    drifts.append(info["drift"])
                    passive_fill_rates.append(info["passive_fill_rate"])

            if (
                args.eval_save_interval > 0
                and len(rewards) >= next_save
            ):
                end = min(len(rewards), args.n_eval_episodes)
                save_evaluation(
                    partial_path,
                    rewards[:end],
                    times[:end],
                    drifts[:end],
                    passive_fill_rates[:end],
                )
                print(f"evaluated: {end}/{args.n_eval_episodes}")
                next_save += args.eval_save_interval
    except KeyboardInterrupt:
        save_evaluation(
            partial_path,
            rewards,
            times,
            drifts,
            passive_fill_rates,
        )
        print(
            f"evaluation interrupted; saved {len(rewards)} episodes "
            f"to {partial_path}"
        )
        raise
    finally:
        envs.close()

    end = args.n_eval_episodes
    rewards = rewards[:end]
    times = times[:end]
    drifts = drifts[:end]
    passive_fill_rates = passive_fill_rates[:end]
    save_evaluation(
        reward_path,
        rewards,
        times,
        drifts,
        passive_fill_rates,
    )

    rewards_array = np.asarray(rewards, dtype=float)
    print(f"saved evaluation to {reward_path}")
    print(f"episodes: {len(rewards_array)}")
    print(f"all finite: {np.isfinite(rewards_array).all()}")
    print(f"mean: {rewards_array.mean()}")
    print(f"std: {rewards_array.std()}")
    print(
        "p05/p50/p95: "
        f"{np.percentile(rewards_array, [5, 50, 95])}"
    )


def validate_and_finalize_args(args):
    if args.drop_feature == "None":
        args.drop_feature = None
    if args.env_type not in {"noise", "flow", "strategic"}:
        raise ValueError(f"unknown market environment: {args.env_type}")
    if args.drop_feature not in {None, "volume", "order_info", "drift"}:
        raise ValueError(f"unknown drop_feature: {args.drop_feature}")
    if args.num_envs < 1 or args.num_steps < 1:
        raise ValueError("num_envs and num_steps must be positive")
    if args.num_minibatches < 1 or args.update_epochs < 1:
        raise ValueError("num_minibatches and update_epochs must be positive")
    if not 0.0 < args.clip_coef < 1.0:
        raise ValueError("clip_coef must be between zero and one")
    if args.max_grad_norm <= 0:
        raise ValueError("max_grad_norm must be positive")
    if args.evaluate and args.n_eval_episodes < 1:
        raise ValueError("n_eval_episodes must be positive when evaluating")

    args.batch_size = args.num_envs * args.num_steps
    if args.batch_size % args.num_minibatches != 0:
        raise ValueError("batch_size must be divisible by num_minibatches")
    args.minibatch_size = args.batch_size // args.num_minibatches
    if args.minibatch_size < 2 and args.norm_adv:
        raise ValueError(
            "minibatch_size must be at least two when normalizing advantages"
        )
    if args.total_timesteps % args.batch_size != 0:
        raise ValueError("total_timesteps must be divisible by batch_size")
    args.num_iterations = args.total_timesteps // args.batch_size
    if args.num_iterations < 2:
        raise ValueError("num_iterations must be at least two")


def generated_run_name(args):
    if args.run_name:
        return args.run_name
    name = (
        f"{args.env_type}_{args.num_lots}_seed_{args.seed}_"
        f"eval_seed_{args.eval_seed}_eval_episodes_{args.n_eval_episodes}_"
        f"num_iterations_{args.num_iterations}_bsize_{args.batch_size}_"
        "ppo_logistic_normal"
    )
    if args.tag:
        name += f"_{args.tag}"
    if args.drop_feature is not None:
        name += f"_{args.drop_feature}"
    return name


def train(args):
    validate_and_finalize_args(args)
    run_name = generated_run_name(args)
    output_root = Path(args.output_dir).expanduser().resolve()
    tensorboard_dir = output_root / "tensorboard_logs" / run_name
    model_dir = output_root / "models"
    reward_dir = output_root / "rewards"
    model_dir.mkdir(parents=True, exist_ok=True)
    reward_dir.mkdir(parents=True, exist_ok=True)

    print("starting PPO logistic-normal training")
    print(f"run name: {run_name}")
    print(
        f"environment={args.env_type}, lots={args.num_lots}, "
        f"batch_size={args.batch_size}, minibatch_size={args.minibatch_size}, "
        f"iterations={args.num_iterations}, epochs={args.update_epochs}"
    )

    writer = SummaryWriter(str(tensorboard_dir))
    writer.add_text(
        "hyperparameters",
        "|param|value|\n|-|-|\n%s"
        % "\n".join(
            f"|{key}|{value}|" for key, value in vars(args).items()
        ),
    )

    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    torch.backends.cudnn.deterministic = args.torch_deterministic
    device = torch.device(
        "cuda" if torch.cuda.is_available() and args.cuda else "cpu"
    )
    print(f"device: {device}")

    envs = None
    try:
        envs = gym.vector.AsyncVectorEnv(
            [make_env(config) for config in build_configs(args)]
        )
        if not isinstance(envs.single_action_space, gym.spaces.Box):
            raise TypeError("only continuous Box action spaces are supported")

        # Keep the same two-reset behavior as actor_critic.py so controlled A/B
        # runs consume the environment RNG streams in the same way.
        initial_observation, _ = envs.reset(seed=args.seed)
        print(
            f"observation shape: {initial_observation.shape}; "
            f"action shape: {envs.single_action_space.shape}"
        )

        agent = AgentLogisticNormal(envs).to(device)
        optimizer = optim.Adam(agent.parameters(), lr=args.learning_rate, eps=1e-5)

        observations = torch.zeros(
            (args.num_steps, args.num_envs)
            + envs.single_observation_space.shape,
            device=device,
        )
        actions = torch.zeros(
            (args.num_steps, args.num_envs) + envs.single_action_space.shape,
            device=device,
        )
        log_probabilities = torch.zeros(
            (args.num_steps, args.num_envs), device=device
        )
        rewards = torch.zeros(
            (args.num_steps, args.num_envs), device=device
        )
        dones = torch.zeros(
            (args.num_steps, args.num_envs), device=device
        )
        values = torch.zeros(
            (args.num_steps, args.num_envs), device=device
        )

        global_step = 0
        start_time = time.time()
        next_observation, _ = envs.reset(seed=args.seed)
        next_observation = torch.as_tensor(
            next_observation, dtype=torch.float32, device=device
        )
        next_done = torch.zeros(args.num_envs, device=device)

        for iteration in range(args.num_iterations):
            episode_returns = []
            episode_times = []
            episode_drifts = []
            passive_fill_rates = []

            if args.anneal_lr:
                fraction = 1.0 - iteration / args.num_iterations
                optimizer.param_groups[0]["lr"] = (
                    fraction * args.learning_rate
                )

            # Preserve the exploration schedule used by actor_critic.py.
            agent.variance = (
                (0.32 - 1.0) * iteration / (args.num_iterations - 1)
                + 1.0
            )

            for step in range(args.num_steps):
                global_step += args.num_envs
                observations[step] = next_observation
                dones[step] = next_done

                with torch.no_grad():
                    action, log_probability, _, value = (
                        agent.get_action_and_value(next_observation)
                    )
                    values[step] = value.flatten()
                actions[step] = action
                log_probabilities[step] = log_probability

                (
                    next_observation_numpy,
                    reward,
                    terminations,
                    truncations,
                    infos,
                ) = envs.step(action.cpu().numpy())
                next_done_numpy = np.logical_or(terminations, truncations)
                rewards[step] = torch.as_tensor(
                    reward, dtype=torch.float32, device=device
                ).view(-1)
                next_observation = torch.as_tensor(
                    next_observation_numpy,
                    dtype=torch.float32,
                    device=device,
                )
                next_done = torch.as_tensor(
                    next_done_numpy,
                    dtype=torch.float32,
                    device=device,
                )

                if "final_info" in infos:
                    for info in infos["final_info"]:
                        if info is None:
                            continue
                        episode_returns.append(info["reward"])
                        episode_times.append(info["time"])
                        episode_drifts.append(info["drift"])
                        passive_fill_rates.append(info["passive_fill_rate"])

            if episode_returns:
                writer.add_scalar(
                    "charts/return", np.mean(episode_returns), global_step
                )
                writer.add_scalar(
                    "charts/time", np.mean(episode_times), global_step
                )
                writer.add_scalar(
                    "charts/drift", np.mean(episode_drifts), global_step
                )
                writer.add_scalar(
                    "charts/passive_fill_rate",
                    np.mean(passive_fill_rates),
                    global_step,
                )
                writer.add_scalar(
                    "charts/early_completion_rate",
                    np.mean(np.asarray(episode_times) < args.terminal_time),
                    global_step,
                )
            else:
                print(
                    f"warning: no episode completed in iteration {iteration}"
                )
            writer.add_scalar(
                "charts/episodes_in_batch", len(episode_returns), global_step
            )

            with torch.no_grad():
                next_value = agent.get_value(next_observation).reshape(1, -1)
                advantages = torch.zeros_like(rewards, device=device)
                last_gae_lambda = 0
                for step in reversed(range(args.num_steps)):
                    if step == args.num_steps - 1:
                        next_nonterminal = 1.0 - next_done
                        next_values = next_value
                    else:
                        next_nonterminal = 1.0 - dones[step + 1]
                        next_values = values[step + 1]
                    delta = (
                        rewards[step]
                        + args.gamma * next_values * next_nonterminal
                        - values[step]
                    )
                    last_gae_lambda = (
                        delta
                        + args.gamma
                        * args.gae_lambda
                        * next_nonterminal
                        * last_gae_lambda
                    )
                    advantages[step] = last_gae_lambda
                return_targets = advantages + values

            batch_observations = observations.reshape(
                (-1,) + envs.single_observation_space.shape
            )
            batch_log_probabilities = log_probabilities.reshape(-1)
            batch_actions = actions.reshape(
                (-1,) + envs.single_action_space.shape
            )
            batch_advantages = advantages.reshape(-1)
            batch_return_targets = return_targets.reshape(-1)
            batch_values = values.reshape(-1)

            batch_indices = np.arange(args.batch_size)
            policy_losses = []
            value_losses = []
            entropy_values = []
            total_losses = []
            old_approx_kls = []
            approx_kls = []
            clip_fractions = []
            gradient_norms = []
            epochs_used = 0

            for epoch in range(args.update_epochs):
                np.random.shuffle(batch_indices)
                epoch_approx_kls = []

                for start in range(0, args.batch_size, args.minibatch_size):
                    end = start + args.minibatch_size
                    minibatch_indices = batch_indices[start:end]

                    _, new_log_probability, entropy, new_value = (
                        agent.get_action_and_value(
                            batch_observations[minibatch_indices],
                            batch_actions[minibatch_indices],
                        )
                    )
                    minibatch_advantages = batch_advantages[minibatch_indices]
                    if args.norm_adv:
                        minibatch_advantages = (
                            minibatch_advantages
                            - minibatch_advantages.mean()
                        ) / (minibatch_advantages.std() + 1e-8)

                    (
                        policy_loss,
                        _,
                        old_approx_kl,
                        approx_kl,
                        clip_fraction,
                    ) = compute_ppo_policy_loss(
                        new_log_probability,
                        batch_log_probabilities[minibatch_indices],
                        minibatch_advantages,
                        args.clip_coef,
                    )

                    new_value = new_value.view(-1)
                    if args.clip_vloss:
                        value_loss_unclipped = (
                            new_value
                            - batch_return_targets[minibatch_indices]
                        ) ** 2
                        value_clipped = batch_values[minibatch_indices] + (
                            new_value - batch_values[minibatch_indices]
                        ).clamp(-args.clip_coef, args.clip_coef)
                        value_loss_clipped = (
                            value_clipped
                            - batch_return_targets[minibatch_indices]
                        ) ** 2
                        value_loss = 0.5 * torch.maximum(
                            value_loss_unclipped,
                            value_loss_clipped,
                        ).mean()
                    else:
                        value_loss = 0.5 * (
                            (
                                new_value
                                - batch_return_targets[minibatch_indices]
                            )
                            ** 2
                        ).mean()

                    entropy_value = entropy.mean()
                    total_loss = (
                        policy_loss
                        - args.ent_coef * entropy_value
                        + args.vf_coef * value_loss
                    )
                    if not torch.isfinite(total_loss):
                        raise FloatingPointError(
                            f"non-finite PPO loss at iteration {iteration}"
                        )

                    optimizer.zero_grad()
                    total_loss.backward()
                    gradient_norm = nn.utils.clip_grad_norm_(
                        agent.parameters(), args.max_grad_norm
                    )
                    optimizer.step()

                    policy_losses.append(policy_loss.item())
                    value_losses.append(value_loss.item())
                    entropy_values.append(entropy_value.item())
                    total_losses.append(total_loss.item())
                    old_approx_kls.append(old_approx_kl.item())
                    approx_kls.append(approx_kl.item())
                    epoch_approx_kls.append(approx_kl.item())
                    clip_fractions.append(clip_fraction.item())
                    gradient_norms.append(float(gradient_norm))

                epochs_used = epoch + 1
                if (
                    args.target_kl is not None
                    and np.mean(epoch_approx_kls) > args.target_kl
                ):
                    break

            predicted_values = batch_values.cpu().numpy()
            target_values = batch_return_targets.cpu().numpy()
            target_variance = np.var(target_values)
            explained_variance = (
                np.nan
                if target_variance == 0
                else 1
                - np.var(target_values - predicted_values) / target_variance
            )

            writer.add_scalar(
                "charts/learning_rate",
                optimizer.param_groups[0]["lr"],
                global_step,
            )
            writer.add_scalar(
                "losses/policy_loss", np.mean(policy_losses), global_step
            )
            writer.add_scalar(
                "losses/value_loss", np.mean(value_losses), global_step
            )
            writer.add_scalar(
                "losses/total_loss", np.mean(total_losses), global_step
            )
            writer.add_scalar(
                "losses/entropy", np.mean(entropy_values), global_step
            )
            writer.add_scalar(
                "losses/old_approx_kl",
                np.mean(old_approx_kls),
                global_step,
            )
            writer.add_scalar(
                "losses/approx_kl", np.mean(approx_kls), global_step
            )
            writer.add_scalar(
                "losses/clipfrac", np.mean(clip_fractions), global_step
            )
            writer.add_scalar(
                "losses/grad_norm", np.mean(gradient_norms), global_step
            )
            writer.add_scalar(
                "losses/explained_variance",
                explained_variance,
                global_step,
            )
            writer.add_scalar(
                "losses/update_epochs", epochs_used, global_step
            )
            writer.add_scalar(
                "values/variance", agent.variance, global_step
            )
            steps_per_second = int(global_step / (time.time() - start_time))
            writer.add_scalar("charts/SPS", steps_per_second, global_step)

            print(
                f"iteration={iteration + 1}/{args.num_iterations}, "
                f"SPS={steps_per_second}, "
                f"approx_kl={np.mean(approx_kls):.6f}, "
                f"clipfrac={np.mean(clip_fractions):.4f}, "
                f"epochs={epochs_used}"
            )
    finally:
        if envs is not None:
            envs.close()
        writer.close()

    model_path = model_dir / f"{run_name}.pt"
    if args.save_model:
        torch.save(agent.state_dict(), model_path)
        print(f"model saved to {model_path}")

    if args.evaluate:
        reward_path = reward_dir / f"{run_name}.npz"
        partial_path = reward_dir / f"{run_name}_partial.npz"
        evaluate_final_policy(
            agent,
            args,
            device,
            reward_path,
            partial_path,
        )

    return run_name


if __name__ == "__main__":
    train(tyro.cli(Args))
