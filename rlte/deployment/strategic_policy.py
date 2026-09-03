"""Inference-only wrapper for the current strategic PPO checkpoint."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np
import torch
import torch.nn as nn

from deployment.schemas import (
    ACTION_DIMENSION,
    CONTRACT_VERSION,
    OBSERVATION_DIMENSION,
    CurrentModelObservation,
    ExecutionIntent,
    validate_observation_array,
    validate_observation_batch,
)


REFERENCE_MODEL_SHA256 = (
    "7c108c25ab2f85335e87fc3bd85de51f"
    "0774128341784223ff593557fc645cf6"
)


class _CompatibleActorCritic(nn.Module):
    """Network layout with keys compatible with the training checkpoint."""

    def __init__(self) -> None:
        super().__init__()
        hidden_size = 128
        self.critic = nn.Sequential(
            nn.Linear(OBSERVATION_DIMENSION, hidden_size),
            nn.Tanh(),
            nn.Linear(hidden_size, hidden_size),
            nn.Tanh(),
            nn.Linear(hidden_size, 1),
        )
        self.actor_mean = nn.Sequential(
            nn.Linear(OBSERVATION_DIMENSION, hidden_size),
            nn.Tanh(),
            nn.Linear(hidden_size, hidden_size),
            nn.Tanh(),
            nn.Linear(hidden_size, ACTION_DIMENSION - 1),
        )

    def deterministic_action(self, observation: torch.Tensor) -> torch.Tensor:
        base_action = self.actor_mean(observation)
        denominator = 1.0 + torch.sum(
            torch.exp(base_action),
            dim=1,
            keepdim=True,
        )
        action = torch.exp(base_action) / denominator
        return torch.cat((action, 1.0 / denominator), dim=1)


def sha256_file(path: str | Path) -> str:
    """Return the SHA-256 digest of a model artifact."""

    digest = hashlib.sha256()
    with Path(path).open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass(frozen=True)
class PolicyMetadata:
    contract_version: str
    model_path: Path
    model_sha256: str
    observation_dimension: int
    action_dimension: int
    device: str
    parameter_count: int


class StrategicExecutionPolicy:
    """Validated deterministic inference API for vn.py integration.

    The class is intentionally unaware of QuickFIX and of the simulator.  It
    performs no scheduling, order reconciliation, risk checks, or order sends.
    """

    def __init__(
        self,
        network: _CompatibleActorCritic,
        metadata: PolicyMetadata,
        device: torch.device,
    ) -> None:
        self._network = network
        self._metadata = metadata
        self._device = device

    @classmethod
    def load(
        cls,
        model_path: str | Path,
        *,
        device: str | torch.device = "cpu",
        expected_sha256: str | None = None,
    ) -> "StrategicExecutionPolicy":
        """Load a strict, weights-only checkpoint.

        CPU is the default because this wrapper is intended for low-latency
        single-observation inference inside a Python execution application.
        """

        path = Path(model_path).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(f"model not found: {path}")

        resolved_device = torch.device(device)
        if resolved_device.type == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA was requested but is not available")

        model_sha256 = sha256_file(path)
        if (
            expected_sha256 is not None
            and model_sha256.lower() != expected_sha256.lower()
        ):
            raise ValueError(
                "model SHA-256 mismatch: "
                f"expected {expected_sha256}, got {model_sha256}"
            )

        try:
            state_dict = torch.load(
                path,
                map_location=resolved_device,
                weights_only=True,
            )
        except TypeError as error:
            raise RuntimeError(
                "this deployment wrapper requires a PyTorch version that "
                "supports torch.load(..., weights_only=True)"
            ) from error

        if not isinstance(state_dict, dict):
            raise TypeError("checkpoint must contain a PyTorch state dictionary")

        network = _CompatibleActorCritic().to(resolved_device)
        try:
            network.load_state_dict(state_dict, strict=True)
        except RuntimeError as error:
            raise ValueError(
                "checkpoint is incompatible with the fixed 67-to-7 policy "
                "architecture"
            ) from error
        network.eval()
        for parameter in network.parameters():
            parameter.requires_grad_(False)

        metadata = PolicyMetadata(
            contract_version=CONTRACT_VERSION,
            model_path=path,
            model_sha256=model_sha256,
            observation_dimension=OBSERVATION_DIMENSION,
            action_dimension=ACTION_DIMENSION,
            device=str(resolved_device),
            parameter_count=sum(
                parameter.numel() for parameter in network.parameters()
            ),
        )
        return cls(network, metadata, resolved_device)

    @property
    def metadata(self) -> PolicyMetadata:
        return self._metadata

    def predict_array(
        self,
        observation: CurrentModelObservation | Sequence[float] | np.ndarray,
    ) -> np.ndarray:
        """Return one deterministic seven-fraction action as float32."""

        if isinstance(observation, CurrentModelObservation):
            array = observation.to_array()
        else:
            array = validate_observation_array(observation)
        return self.predict_batch_array(array.reshape(1, -1))[0]

    def predict(
        self,
        observation: CurrentModelObservation | Sequence[float] | np.ndarray,
    ) -> ExecutionIntent:
        """Return one deterministic action as a structured intent."""

        return ExecutionIntent.from_array(self.predict_array(observation))

    def predict_batch_array(
        self,
        observations: Sequence[Sequence[float]] | np.ndarray,
    ) -> np.ndarray:
        """Return deterministic actions for a validated observation batch."""

        batch = validate_observation_batch(observations)
        tensor = torch.as_tensor(
            batch,
            dtype=torch.float32,
            device=self._device,
        )
        with torch.inference_mode():
            actions = self._network.deterministic_action(tensor)
        output = actions.detach().cpu().numpy().astype(np.float32, copy=False)

        if output.shape != (len(batch), ACTION_DIMENSION):
            raise RuntimeError(f"policy returned unexpected shape {output.shape}")
        if not np.isfinite(output).all():
            raise FloatingPointError("policy returned a non-finite action")
        if np.any(output < 0):
            raise FloatingPointError("policy returned a negative action fraction")
        if not np.allclose(output.sum(axis=1), 1.0, atol=1e-5):
            raise FloatingPointError("policy action fractions do not sum to one")
        return output.copy()

    def predict_batch(
        self,
        observations: Sequence[Sequence[float]] | np.ndarray,
    ) -> list[ExecutionIntent]:
        """Return a list of structured intents for a batch."""

        return [
            ExecutionIntent.from_array(action)
            for action in self.predict_batch_array(observations)
        ]
