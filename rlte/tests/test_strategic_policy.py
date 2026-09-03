from pathlib import Path
from types import SimpleNamespace

import gymnasium as gym
import numpy as np
import pytest
import torch

from deployment.schemas import (
    FEATURE_NAMES,
    CurrentModelObservation,
    ExecutionIntent,
)
from deployment.strategic_policy import (
    REFERENCE_MODEL_SHA256,
    StrategicExecutionPolicy,
    sha256_file,
)
from rl_files.ppo_logistic_normal import AgentLogisticNormal


PROJECT_ROOT = Path(__file__).resolve().parents[1]
REFERENCE_MODEL = (
    PROJECT_ROOT
    / "models"
    / (
        "strategic_20_seed_0_eval_seed_100_eval_episodes_1000_"
        "num_iterations_500_bsize_400_"
        "ppo_logistic_normal_comparison_200k.pt"
    )
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


def save_training_checkpoint(path: Path) -> AgentLogisticNormal:
    torch.manual_seed(7)
    training_agent = AgentLogisticNormal(make_dummy_vector_environment())
    torch.save(training_agent.state_dict(), path)
    return training_agent


def test_inference_wrapper_matches_training_model(tmp_path):
    checkpoint = tmp_path / "policy.pt"
    training_agent = save_training_checkpoint(checkpoint)
    inference_policy = StrategicExecutionPolicy.load(
        checkpoint,
        expected_sha256=sha256_file(checkpoint),
    )
    observations = np.random.default_rng(9).normal(size=(8, 67)).astype(
        np.float32
    )

    with torch.no_grad():
        expected = training_agent.deterministic_action(
            torch.as_tensor(observations)
        ).numpy()
    actual = inference_policy.predict_batch_array(observations)

    np.testing.assert_allclose(actual, expected, rtol=1e-6, atol=1e-7)
    np.testing.assert_allclose(actual.sum(axis=1), 1.0, atol=1e-6)
    assert np.all(actual > 0)
    assert inference_policy.metadata.observation_dimension == 67
    assert inference_policy.metadata.action_dimension == 7
    assert inference_policy.metadata.parameter_count == 51_335


def test_single_and_batch_prediction_are_consistent(tmp_path):
    checkpoint = tmp_path / "policy.pt"
    save_training_checkpoint(checkpoint)
    policy = StrategicExecutionPolicy.load(checkpoint)
    observation = np.linspace(-1.0, 1.0, 67, dtype=np.float32)

    single_array = policy.predict_array(observation)
    batch_array = policy.predict_batch_array(observation.reshape(1, -1))[0]
    intent = policy.predict(observation)

    np.testing.assert_allclose(single_array, batch_array, atol=0, rtol=0)
    np.testing.assert_allclose(intent.to_array(), single_array, atol=1e-7)


def test_structured_observation_has_exact_contract_order():
    observation = CurrentModelObservation(
        normalized_time=0.5,
        remaining_inventory_fraction=0.75,
        best_bid_drift=0.1,
        mid_price_drift=0.2,
        normalized_spread=0.1,
        book_imbalance=-0.3,
        normalized_bid_depth=[1, 2, 3, 4, 5],
        normalized_ask_depth=[6, 7, 8, 9, 10],
        own_order_distribution=[0, 0, 0, 0, 0, 0, 1],
        inventory_unit_level_codes=np.linspace(-1, 1, 20),
        inventory_unit_queue_codes=np.linspace(1, -1, 20),
        recent_market_order_imbalance=0.4,
        recent_limit_order_imbalance=0.5,
        recent_cancellation_imbalance=-0.6,
        recent_mid_price_drift=0.7,
    )

    array = observation.to_array()

    assert array.shape == (67,)
    assert len(FEATURE_NAMES) == 67
    np.testing.assert_allclose(array[:6], [0.5, 0.75, 0.1, 0.2, 0.1, -0.3])
    np.testing.assert_allclose(array[6:11], [1, 2, 3, 4, 5])
    np.testing.assert_allclose(array[11:16], [6, 7, 8, 9, 10])
    np.testing.assert_allclose(array[16:23], [0, 0, 0, 0, 0, 0, 1])
    np.testing.assert_allclose(array[63:], [0.4, 0.5, -0.6, 0.7])


def test_simulator_target_quantities_conserve_inventory():
    intent = ExecutionIntent.from_array(
        [0.10, 0.20, 0.15, 0.10, 0.05, 0.10, 0.30]
    )

    allocation = intent.simulator_target_quantities(20)

    assert allocation.market_sell == 2
    assert allocation.limit_sell_levels == (4, 3, 2, 1, 2)
    assert allocation.inactive == 6
    assert allocation.total == 20
    np.testing.assert_array_equal(allocation.to_array(), [2, 4, 3, 2, 1, 2, 6])


def test_invalid_inputs_and_hash_are_rejected(tmp_path):
    checkpoint = tmp_path / "policy.pt"
    save_training_checkpoint(checkpoint)

    with pytest.raises(ValueError, match="SHA-256 mismatch"):
        StrategicExecutionPolicy.load(checkpoint, expected_sha256="0" * 64)

    policy = StrategicExecutionPolicy.load(checkpoint)
    with pytest.raises(ValueError, match="shape"):
        policy.predict_array(np.zeros(66, dtype=np.float32))
    with pytest.raises(ValueError, match="finite"):
        bad_observation = np.zeros(67, dtype=np.float32)
        bad_observation[4] = np.nan
        policy.predict_array(bad_observation)
    with pytest.raises(ValueError, match="must not be empty"):
        policy.predict_batch_array(np.empty((0, 67), dtype=np.float32))


def test_reference_model_hash_and_load_when_artifact_is_available():
    if not REFERENCE_MODEL.is_file():
        pytest.skip("git-ignored reference model is not present")

    assert sha256_file(REFERENCE_MODEL) == REFERENCE_MODEL_SHA256
    policy = StrategicExecutionPolicy.load(
        REFERENCE_MODEL,
        expected_sha256=REFERENCE_MODEL_SHA256,
    )
    assert policy.metadata.model_sha256 == REFERENCE_MODEL_SHA256
    assert policy.metadata.parameter_count == 51_335

    training_agent = AgentLogisticNormal(make_dummy_vector_environment())
    training_agent.load_state_dict(
        torch.load(REFERENCE_MODEL, map_location="cpu", weights_only=True)
    )
    training_agent.eval()
    observations = np.random.default_rng(100).normal(size=(4, 67)).astype(
        np.float32
    )
    with torch.no_grad():
        expected = training_agent.deterministic_action(
            torch.as_tensor(observations)
        ).numpy()
    actual = policy.predict_batch_array(observations)
    np.testing.assert_allclose(actual, expected, rtol=1e-6, atol=1e-7)
