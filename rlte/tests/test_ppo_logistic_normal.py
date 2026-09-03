from types import SimpleNamespace

import gymnasium as gym
import numpy as np
import torch

from rl_files.actor_critic import AgentLogisticNormal as ActorCriticAgent
from rl_files.ppo_logistic_normal import (
    AgentLogisticNormal as PPOAgent,
    compute_ppo_policy_loss,
)


def make_dummy_vector_environment():
    return SimpleNamespace(
        single_observation_space=gym.spaces.Box(
            low=-np.inf,
            high=np.inf,
            shape=(67,),
            dtype=np.float32,
        ),
        single_action_space=gym.spaces.Box(
            low=0.0,
            high=1.0,
            shape=(7,),
            dtype=np.float32,
        ),
    )


def test_ppo_ratio_is_one_before_policy_changes():
    old_log_probability = torch.tensor([-0.2, -1.0, -2.5])
    new_log_probability = old_log_probability.clone().requires_grad_()
    advantages = torch.tensor([1.0, -0.5, 0.25])

    (
        policy_loss,
        ratio,
        old_approx_kl,
        approx_kl,
        clip_fraction,
    ) = compute_ppo_policy_loss(
        new_log_probability,
        old_log_probability,
        advantages,
        clip_coef=0.2,
    )

    torch.testing.assert_close(ratio, torch.ones_like(ratio))
    torch.testing.assert_close(old_approx_kl, torch.tensor(0.0))
    torch.testing.assert_close(approx_kl, torch.tensor(0.0))
    torch.testing.assert_close(clip_fraction, torch.tensor(0.0))
    torch.testing.assert_close(policy_loss, -advantages.mean())


def test_ppo_policy_loss_clips_large_probability_changes():
    old_log_probability = torch.zeros(2)
    new_log_probability = torch.log(torch.tensor([1.5, 0.5])).requires_grad_()
    advantages = torch.tensor([1.0, -1.0])

    (
        policy_loss,
        ratio,
        _,
        _,
        clip_fraction,
    ) = compute_ppo_policy_loss(
        new_log_probability,
        old_log_probability,
        advantages,
        clip_coef=0.2,
    )

    torch.testing.assert_close(ratio, torch.tensor([1.5, 0.5]))
    # Positive advantage is capped at ratio 1.2; negative advantage is capped
    # at ratio 0.8. The two per-sample losses are therefore -1.2 and 0.8.
    torch.testing.assert_close(policy_loss, torch.tensor(-0.2))
    torch.testing.assert_close(clip_fraction, torch.tensor(1.0))

    policy_loss.backward()
    assert new_log_probability.grad is not None
    assert torch.isfinite(new_log_probability.grad).all()


def test_logistic_normal_actions_stay_on_the_simplex():
    envs = make_dummy_vector_environment()
    agent = PPOAgent(envs)
    observations = torch.randn(8, 67)

    sampled_actions, _, _, _ = agent.get_action_and_value(observations)
    deterministic_actions = agent.deterministic_action(observations)

    for actions in (sampled_actions, deterministic_actions):
        assert actions.shape == (8, 7)
        assert torch.all(actions > 0)
        torch.testing.assert_close(
            actions.sum(dim=1),
            torch.ones(8),
            atol=1e-6,
            rtol=1e-6,
        )


def test_ppo_agent_can_load_actor_critic_weights():
    envs = make_dummy_vector_environment()
    actor_critic_agent = ActorCriticAgent(envs)
    ppo_agent = PPOAgent(envs)

    ppo_agent.load_state_dict(actor_critic_agent.state_dict())

    for key, value in actor_critic_agent.state_dict().items():
        torch.testing.assert_close(ppo_agent.state_dict()[key], value)
