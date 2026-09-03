from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Sequence

import numpy as np
from vnpy.trader.constant import Direction, Exchange

from .constants import MODEL_DECISION_COUNT, MODEL_INVENTORY


class ParentStatus(Enum):
    WAITING = "WAITING"
    RUNNING = "RUNNING"
    PAUSED = "PAUSED"
    COMPLETED = "COMPLETED"
    CANCELLED = "CANCELLED"
    ERROR = "ERROR"


class TrancheStatus(Enum):
    WAITING = "WAITING"
    ACTIVE = "ACTIVE"
    CLOSING = "CLOSING"
    COMPLETED = "COMPLETED"
    CANCELLED = "CANCELLED"
    ERROR = "ERROR"


@dataclass(frozen=True)
class AcParameters:
    volatility: float = 0.02
    temporary_impact: float = 0.1
    risk_aversion: float = 0.0

    def __post_init__(self) -> None:
        if self.volatility < 0:
            raise ValueError("volatility must be non-negative")
        if self.temporary_impact <= 0:
            raise ValueError("temporary_impact must be positive")
        if self.risk_aversion < 0:
            raise ValueError("risk_aversion must be non-negative")


@dataclass(frozen=True)
class ParentOrderRequest:
    """Execution request plus adapter metadata outside the model feature vector.

    In particular, ``price_tick`` converts real prices to the integer-tick
    units used during training.  It is not, and must never become, a 68th
    observation feature.
    """

    symbol: str
    exchange: Exchange
    total_volume: int
    duration_seconds: float
    rl_horizon_seconds: float
    max_active_tranches: int
    gateway_name: str = "QUICKFIX"
    direction: Direction = Direction.SHORT
    price_tick: float = 0.01
    use_synthetic_market: bool = True
    max_market_age_seconds: float = 5.0
    ac: AcParameters = field(default_factory=AcParameters)

    def __post_init__(self) -> None:
        if not self.symbol:
            raise ValueError("symbol is required")
        if self.direction != Direction.SHORT:
            raise ValueError("the current RL model only supports sell orders")
        if self.total_volume <= 0:
            raise ValueError("total_volume must be positive")
        if self.total_volume % MODEL_INVENTORY:
            raise ValueError(
                f"total_volume must be a multiple of {MODEL_INVENTORY}"
            )
        if self.duration_seconds <= 0:
            raise ValueError("duration_seconds must be positive")
        if self.rl_horizon_seconds <= 0:
            raise ValueError("rl_horizon_seconds must be positive")
        if self.max_active_tranches <= 0:
            raise ValueError("max_active_tranches must be positive")
        if self.price_tick <= 0:
            raise ValueError("price_tick must be positive")
        if self.max_market_age_seconds <= 0:
            raise ValueError("max_market_age_seconds must be positive")


@dataclass
class Tranche:
    tranche_id: str
    parent_id: str
    sequence: int
    planned_start_offset: float
    deadline_offset: float
    volume: int = MODEL_INVENTORY
    status: TrancheStatus = TrancheStatus.WAITING
    started_at: float | None = None
    deadline_at: float | None = None
    next_decision_at: float | None = None
    decision_index: int = 0
    traded: int = 0
    reference_bid: float | None = None
    reference_mid: float | None = None
    message: str = ""

    @property
    def remaining(self) -> int:
        return max(self.volume - self.traded, 0)

    @property
    def is_live(self) -> bool:
        return self.status in {TrancheStatus.ACTIVE, TrancheStatus.CLOSING}

    def activate(self, now: float, parent_started_at: float) -> None:
        if self.status != TrancheStatus.WAITING:
            raise RuntimeError(f"cannot activate tranche in {self.status.value}")

        self.started_at = now
        self.deadline_at = parent_started_at + self.deadline_offset
        self.next_decision_at = now
        self.status = TrancheStatus.ACTIVE

    def normalized_time(self, now: float) -> float:
        if self.started_at is None or self.deadline_at is None:
            raise RuntimeError("tranche has not started")
        horizon = self.deadline_at - self.started_at
        if horizon <= 0:
            return 1.0
        return min(max((now - self.started_at) / horizon, 0.0), 1.0)

    def decision_due(self, now: float) -> bool:
        return (
            self.status == TrancheStatus.ACTIVE
            and self.decision_index < MODEL_DECISION_COUNT
            and self.next_decision_at is not None
            and now >= self.next_decision_at
            and (self.deadline_at is None or now < self.deadline_at)
        )

    def record_decision(self) -> None:
        if self.started_at is None or self.deadline_at is None:
            raise RuntimeError("tranche has not started")
        self.decision_index += 1
        if self.decision_index >= MODEL_DECISION_COUNT:
            self.next_decision_at = None
            return

        horizon = max(self.deadline_at - self.started_at, 0.0)
        interval = horizon / MODEL_DECISION_COUNT
        self.next_decision_at = self.started_at + self.decision_index * interval


@dataclass
class ParentOrder:
    parent_id: str
    request: ParentOrderRequest
    created_at: float
    started_at: float
    tranches: list[Tranche]
    status: ParentStatus = ParentStatus.RUNNING
    traded: int = 0
    message: str = ""

    @property
    def deadline_at(self) -> float:
        return self.started_at + self.request.duration_seconds

    @property
    def remaining(self) -> int:
        return max(self.request.total_volume - self.traded, 0)

    @property
    def active_tranches(self) -> list[Tranche]:
        return [tranche for tranche in self.tranches if tranche.is_live]


@dataclass(frozen=True)
class ExecutionIntent:
    market_sell_fraction: float
    limit_sell_fractions: tuple[float, float, float, float, float]
    inactive_fraction: float

    @classmethod
    def from_array(cls, values: Sequence[float] | np.ndarray) -> "ExecutionIntent":
        vector = np.asarray(values, dtype=np.float32)
        if vector.shape != (7,):
            raise ValueError(f"intent must have shape (7,), got {vector.shape}")
        if not np.isfinite(vector).all() or np.any(vector < 0):
            raise ValueError("intent must contain finite non-negative values")
        if not np.isclose(vector.sum(), 1.0, atol=1e-5):
            raise ValueError("intent fractions must sum to one")
        return cls(
            market_sell_fraction=float(vector[0]),
            limit_sell_fractions=tuple(float(value) for value in vector[1:6]),
            inactive_fraction=float(vector[6]),
        )

    def to_array(self) -> np.ndarray:
        return np.asarray(
            [
                self.market_sell_fraction,
                *self.limit_sell_fractions,
                self.inactive_fraction,
            ],
            dtype=np.float32,
        )

    def target_quantities(self, remaining: int) -> "TargetAllocation":
        if remaining < 0:
            raise ValueError("remaining must be non-negative")

        available = remaining
        targets: list[int] = []
        for fraction in self.to_array():
            quantity = min(int(np.round(float(fraction) * remaining)), available)
            targets.append(quantity)
            available -= quantity
        targets[-1] += available
        return TargetAllocation(
            market_sell=targets[0],
            limit_sell_levels=tuple(targets[1:6]),
            inactive=targets[6],
        )


@dataclass(frozen=True)
class TargetAllocation:
    market_sell: int
    limit_sell_levels: tuple[int, int, int, int, int]
    inactive: int

    @property
    def total(self) -> int:
        return self.market_sell + sum(self.limit_sell_levels) + self.inactive


@dataclass(frozen=True)
class TrancheOrderState:
    """Model-facing own-order state for the five training observation levels.

    These levels are anchored at best ask, matching RLTE's historical
    ``start_at_best_price=True`` observation path.  They are deliberately not
    the action levels, which are anchored at ``best_bid + one tick``.
    """

    active_by_level: tuple[int, int, int, int, int]
    active_outside: int
    pending_aggressive: int
    queue_ahead_by_level: tuple[float, float, float, float, float]

    @property
    def active_total(self) -> int:
        return sum(self.active_by_level) + self.active_outside + self.pending_aggressive


@dataclass(frozen=True)
class IntentRecord:
    parent_id: str
    tranche_id: str
    normalized_time: float
    remaining: int
    intent: ExecutionIntent
    target: TargetAllocation
