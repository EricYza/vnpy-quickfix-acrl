"""Versioned input and output schemas for the current strategic PPO model.

These types describe the model boundary only.  They intentionally contain no
FIX session, order identifier, or transport logic.  A vn.py execution adapter
is expected to construct a consistent observation snapshot, call the policy,
and reconcile the returned target allocation with live and pending orders.
"""

from __future__ import annotations

from dataclasses import dataclass
from numbers import Integral
from typing import Sequence

import numpy as np


CONTRACT_VERSION = "rlte-strategic-20-v1"
OBSERVATION_DIMENSION = 67
ACTION_DIMENSION = 7
BOOK_LEVELS = 5
MODEL_INVENTORY = 20


FEATURE_NAMES = (
    "normalized_time",
    "remaining_inventory_fraction",
    "best_bid_drift",
    "mid_price_drift",
    "normalized_spread",
    "book_imbalance",
    *(f"normalized_bid_depth_{level}" for level in range(1, 6)),
    *(f"normalized_ask_depth_{level}" for level in range(1, 6)),
    *(f"own_order_fraction_level_{level}" for level in range(1, 6)),
    "own_order_fraction_outside_levels",
    "own_order_fraction_inactive",
    *(f"inventory_unit_level_code_{unit}" for unit in range(1, 21)),
    *(f"inventory_unit_queue_code_{unit}" for unit in range(1, 21)),
    "recent_market_order_imbalance",
    "recent_limit_order_imbalance",
    "recent_cancellation_imbalance",
    "recent_mid_price_drift",
)

ACTION_NAMES = (
    "market_sell_fraction",
    "limit_sell_level_1_fraction",
    "limit_sell_level_2_fraction",
    "limit_sell_level_3_fraction",
    "limit_sell_level_4_fraction",
    "limit_sell_level_5_fraction",
    "inactive_fraction",
)

if len(FEATURE_NAMES) != OBSERVATION_DIMENSION:
    raise RuntimeError("feature-name count does not match observation dimension")
if len(ACTION_NAMES) != ACTION_DIMENSION:
    raise RuntimeError("action-name count does not match action dimension")


def _finite_vector(
    values: Sequence[float] | np.ndarray,
    expected_length: int,
    name: str,
) -> np.ndarray:
    vector = np.asarray(values, dtype=np.float32)
    if vector.shape != (expected_length,):
        raise ValueError(
            f"{name} must have shape ({expected_length},), got {vector.shape}"
        )
    if not np.isfinite(vector).all():
        raise ValueError(f"{name} must contain only finite values")
    return vector


def validate_observation_array(
    observation: Sequence[float] | np.ndarray,
) -> np.ndarray:
    """Return a validated float32 copy of one model-ready observation."""

    return _finite_vector(
        observation,
        OBSERVATION_DIMENSION,
        "observation",
    ).copy()


def validate_observation_batch(
    observations: Sequence[Sequence[float]] | np.ndarray,
) -> np.ndarray:
    """Return a validated float32 copy of a batch of observations."""

    batch = np.asarray(observations, dtype=np.float32)
    if batch.ndim != 2 or batch.shape[1] != OBSERVATION_DIMENSION:
        raise ValueError(
            "observation batch must have shape "
            f"(N, {OBSERVATION_DIMENSION}), got {batch.shape}"
        )
    if batch.shape[0] < 1:
        raise ValueError("observation batch must not be empty")
    if not np.isfinite(batch).all():
        raise ValueError("observation batch must contain only finite values")
    return batch.copy()


@dataclass(frozen=True)
class CurrentModelObservation:
    """Structured representation of the exact 67 features used in training.

    This schema is deliberately fixed to an initial inventory of 20.  It is
    not the future variable-inventory schema; changing the tuple lengths would
    make the saved model weights incompatible.
    """

    normalized_time: float
    remaining_inventory_fraction: float
    best_bid_drift: float
    mid_price_drift: float
    normalized_spread: float
    book_imbalance: float
    normalized_bid_depth: Sequence[float]
    normalized_ask_depth: Sequence[float]
    own_order_distribution: Sequence[float]
    inventory_unit_level_codes: Sequence[float]
    inventory_unit_queue_codes: Sequence[float]
    recent_market_order_imbalance: float
    recent_limit_order_imbalance: float
    recent_cancellation_imbalance: float
    recent_mid_price_drift: float

    def to_array(self) -> np.ndarray:
        bid_depth = _finite_vector(
            self.normalized_bid_depth,
            BOOK_LEVELS,
            "normalized_bid_depth",
        )
        ask_depth = _finite_vector(
            self.normalized_ask_depth,
            BOOK_LEVELS,
            "normalized_ask_depth",
        )
        own_orders = _finite_vector(
            self.own_order_distribution,
            7,
            "own_order_distribution",
        )
        level_codes = _finite_vector(
            self.inventory_unit_level_codes,
            MODEL_INVENTORY,
            "inventory_unit_level_codes",
        )
        queue_codes = _finite_vector(
            self.inventory_unit_queue_codes,
            MODEL_INVENTORY,
            "inventory_unit_queue_codes",
        )

        scalars = np.asarray(
            [
                self.normalized_time,
                self.remaining_inventory_fraction,
                self.best_bid_drift,
                self.mid_price_drift,
                self.normalized_spread,
                self.book_imbalance,
            ],
            dtype=np.float32,
        )
        trailing = np.asarray(
            [
                self.recent_market_order_imbalance,
                self.recent_limit_order_imbalance,
                self.recent_cancellation_imbalance,
                self.recent_mid_price_drift,
            ],
            dtype=np.float32,
        )
        observation = np.concatenate(
            [
                scalars,
                bid_depth,
                ask_depth,
                own_orders,
                level_codes,
                queue_codes,
                trailing,
            ]
        )
        observation = validate_observation_array(observation)

        if not 0.0 <= float(observation[0]) <= 1.0:
            raise ValueError("normalized_time must be in [0, 1]")
        if not 0.0 <= float(observation[1]) <= 1.0:
            raise ValueError("remaining_inventory_fraction must be in [0, 1]")
        if np.any(bid_depth < 0) or np.any(ask_depth < 0):
            raise ValueError("normalized book depths must be non-negative")
        if np.any(own_orders < 0):
            raise ValueError("own_order_distribution must be non-negative")
        if not np.isclose(own_orders.sum(), 1.0, atol=1e-5):
            raise ValueError("own_order_distribution must sum to one")
        return observation


@dataclass(frozen=True)
class TargetAllocation:
    """Integer target quantities produced from an execution intent.

    Limit quantities are target live quantities at the five price levels, not
    unconditional new-order quantities.  A FIX adapter must reconcile them
    against acknowledged live orders, pending cancels, partial fills, and
    rejected requests before sending any command.
    """

    market_sell: int
    limit_sell_levels: tuple[int, int, int, int, int]
    inactive: int

    @property
    def total(self) -> int:
        return self.market_sell + sum(self.limit_sell_levels) + self.inactive

    def to_array(self) -> np.ndarray:
        return np.asarray(
            [
                self.market_sell,
                *self.limit_sell_levels,
                self.inactive,
            ],
            dtype=np.int64,
        )


@dataclass(frozen=True)
class ExecutionIntent:
    """Seven target fractions emitted by the deterministic policy."""

    market_sell_fraction: float
    limit_sell_level_1_fraction: float
    limit_sell_level_2_fraction: float
    limit_sell_level_3_fraction: float
    limit_sell_level_4_fraction: float
    limit_sell_level_5_fraction: float
    inactive_fraction: float

    @classmethod
    def from_array(
        cls,
        action: Sequence[float] | np.ndarray,
    ) -> "ExecutionIntent":
        vector = _finite_vector(action, ACTION_DIMENSION, "action")
        if np.any(vector < 0):
            raise ValueError("action fractions must be non-negative")
        if not np.isclose(vector.sum(), 1.0, atol=1e-5):
            raise ValueError("action fractions must sum to one")
        return cls(*(float(value) for value in vector))

    def to_array(self) -> np.ndarray:
        return _finite_vector(
            [
                self.market_sell_fraction,
                self.limit_sell_level_1_fraction,
                self.limit_sell_level_2_fraction,
                self.limit_sell_level_3_fraction,
                self.limit_sell_level_4_fraction,
                self.limit_sell_level_5_fraction,
                self.inactive_fraction,
            ],
            ACTION_DIMENSION,
            "action",
        ).copy()

    @property
    def limit_sell_fractions(self) -> tuple[float, float, float, float, float]:
        return (
            self.limit_sell_level_1_fraction,
            self.limit_sell_level_2_fraction,
            self.limit_sell_level_3_fraction,
            self.limit_sell_level_4_fraction,
            self.limit_sell_level_5_fraction,
        )

    def simulator_target_quantities(
        self,
        remaining_inventory: int,
    ) -> TargetAllocation:
        """Apply the simulator's exact sequential rounding rule.

        This reproduces ``RLAgent.generate_order``.  It does not reconcile FIX
        order state and must not be treated as a ready-to-send order list.
        """

        if isinstance(remaining_inventory, bool) or not isinstance(
            remaining_inventory,
            Integral,
        ):
            raise TypeError("remaining_inventory must be an integer")
        remaining_inventory = int(remaining_inventory)
        if remaining_inventory < 0:
            raise ValueError("remaining_inventory must be non-negative")

        fractions = self.to_array()
        if np.any(fractions < 0) or not np.isclose(
            fractions.sum(),
            1.0,
            atol=1e-5,
        ):
            raise ValueError("execution intent is not a valid simplex action")

        available = remaining_inventory
        targets: list[int] = []
        for fraction in fractions:
            quantity = min(
                int(np.round(float(fraction) * remaining_inventory)),
                available,
            )
            targets.append(quantity)
            available -= quantity
        targets[-1] += available

        allocation = TargetAllocation(
            market_sell=targets[0],
            limit_sell_levels=tuple(targets[1:6]),
            inactive=targets[6],
        )
        if allocation.total != remaining_inventory:
            raise RuntimeError("target allocation does not conserve inventory")
        return allocation
