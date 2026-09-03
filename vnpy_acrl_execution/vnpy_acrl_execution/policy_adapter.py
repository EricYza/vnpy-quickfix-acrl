from __future__ import annotations

import sys
from pathlib import Path
from typing import Protocol, Sequence

import numpy as np

from .constants import (
    ACTION_DIMENSION,
    EXPECTED_ACTION_NAMES,
    EXPECTED_FEATURE_NAMES,
    MODEL_CONTRACT_VERSION,
    OBSERVATION_DIMENSION,
)
from .models import ExecutionIntent


class ExecutionPolicy(Protocol):
    def predict(self, observation: Sequence[float] | np.ndarray) -> ExecutionIntent:
        ...


class RltePolicyAdapter:
    def __init__(self, policy: object, model_path: Path) -> None:
        self._policy = policy
        self.model_path = model_path

    @classmethod
    def load(
        cls,
        rlte_root: str | Path,
        model_path: str | Path,
        *,
        expected_sha256: str | None = None,
    ) -> "RltePolicyAdapter":
        root = Path(rlte_root).expanduser().resolve()
        model = Path(model_path).expanduser().resolve()
        if not root.is_dir():
            raise FileNotFoundError(f"RLTE root not found: {root}")
        if not model.is_file():
            raise FileNotFoundError(f"RL model not found: {model}")

        root_text = str(root)
        if root_text not in sys.path:
            sys.path.insert(0, root_text)

        from deployment.schemas import (
            ACTION_DIMENSION as rlte_action_dimension,
            ACTION_NAMES as rlte_action_names,
            CONTRACT_VERSION as rlte_contract_version,
            FEATURE_NAMES as rlte_feature_names,
            OBSERVATION_DIMENSION as rlte_observation_dimension,
        )
        from deployment.strategic_policy import StrategicExecutionPolicy

        actual_contract = (
            rlte_contract_version,
            rlte_observation_dimension,
            tuple(rlte_feature_names),
            rlte_action_dimension,
            tuple(rlte_action_names),
        )
        expected_contract = (
            MODEL_CONTRACT_VERSION,
            OBSERVATION_DIMENSION,
            EXPECTED_FEATURE_NAMES,
            ACTION_DIMENSION,
            EXPECTED_ACTION_NAMES,
        )
        if actual_contract != expected_contract:
            raise RuntimeError(
                "RLTE model contract differs from the locked 67-feature/7-action "
                "semantics; refusing inference"
            )

        return cls(
            StrategicExecutionPolicy.load(
                model,
                device="cpu",
                expected_sha256=expected_sha256,
            ),
            model,
        )

    def predict(self, observation: Sequence[float] | np.ndarray) -> ExecutionIntent:
        action = self._policy.predict_array(observation)
        return ExecutionIntent.from_array(action)

    def predict_batch(
        self,
        observations: Sequence[Sequence[float]] | np.ndarray,
    ) -> list[ExecutionIntent]:
        actions = self._policy.predict_batch_array(observations)
        return [ExecutionIntent.from_array(action) for action in actions]


class RuleBasedPolicy:
    """Deterministic fallback used to test the execution chain without a model."""

    def predict(self, observation: Sequence[float] | np.ndarray) -> ExecutionIntent:
        values = np.asarray(observation, dtype=np.float32)
        if values.shape != (OBSERVATION_DIMENSION,):
            raise ValueError(
                f"observation must have shape ({OBSERVATION_DIMENSION},), "
                f"got {values.shape}"
            )
        time_progress = float(values[0])
        remaining_fraction = float(values[1])

        urgency = max(time_progress, remaining_fraction - (1.0 - time_progress))
        if urgency >= 0.8:
            action = [0.70, 0.25, 0.0, 0.0, 0.0, 0.0, 0.05]
        elif urgency >= 0.5:
            action = [0.35, 0.45, 0.10, 0.0, 0.0, 0.0, 0.10]
        else:
            action = [0.10, 0.45, 0.20, 0.10, 0.05, 0.0, 0.10]
        return ExecutionIntent.from_array(action)
