from __future__ import annotations

from pathlib import Path

import numpy as np

from .constants import BOOK_LEVELS, MODEL_INVENTORY, OBSERVATION_DIMENSION
from .market_state import MarketSnapshot
from .models import Tranche, TrancheOrderState


class ObservationBuilder:
    """Reproduce the fixed RLTE ``rlte-strategic-20-v1`` observation contract.

    ``market_tick_size`` is adapter metadata, not an additional model feature.
    The simulator stores prices as integer ticks and divides price differences
    by 10.  A real price difference is therefore converted with
    ``difference / market_tick_size / 10`` before occupying the same slot.
    """

    def __init__(self, depth_normalization: np.ndarray) -> None:
        # RLTE loads the npz arrays as float64 and casts only while assembling
        # the final observation.  Retaining that order avoids extra rounding.
        normalization = np.asarray(depth_normalization, dtype=np.float64)
        if normalization.ndim != 1 or len(normalization) < BOOK_LEVELS:
            raise ValueError("depth normalization must contain at least five values")
        if not np.isfinite(normalization).all() or np.any(normalization <= 0):
            raise ValueError("depth normalization must be finite and positive")
        self.depth_normalization = normalization

    @classmethod
    def from_shape_file(cls, path: str | Path) -> "ObservationBuilder":
        with np.load(Path(path)) as shape:
            normalization = (shape["bidv"] + shape["askv"]) / 2.0
        return cls(normalization)

    @classmethod
    def constant(cls, value: float = 20.0) -> "ObservationBuilder":
        return cls(np.full(BOOK_LEVELS, value, dtype=np.float64))

    def build(
        self,
        tranche: Tranche,
        market: MarketSnapshot,
        orders: TrancheOrderState,
        now: float,
        market_tick_size: float,
    ) -> np.ndarray:
        if tranche.remaining <= 0:
            raise ValueError("cannot build an observation for a completed tranche")
        if market.best_bid <= 0 or market.best_ask <= 0:
            raise ValueError("best bid and ask must be positive")
        if market.best_ask <= market.best_bid:
            raise ValueError("best ask must be greater than best bid")
        if market_tick_size <= 0:
            raise ValueError("market_tick_size must be positive")

        if tranche.reference_bid is None:
            tranche.reference_bid = market.best_bid
        if tranche.reference_mid is None:
            tranche.reference_mid = market.mid_price

        model_bid, model_ask = market.model_book_depths(market_tick_size)
        raw_bid = np.asarray(model_bid, dtype=np.float64)
        raw_ask = np.asarray(model_ask, dtype=np.float64)
        depth_total = float(np.sum(raw_bid + raw_ask))
        imbalance = float(np.sum(raw_bid - raw_ask) / depth_total) if depth_total else 0.0

        # Training prices were integer ticks and all price deltas used / 10.
        price_scale = market_tick_size * 10.0
        normalized_spread = (market.best_ask - market.best_bid) / price_scale
        # Preserve RLTE's historical int(spread - 1) indexing exactly.
        shape_index = int(normalized_spread - 1)
        shape = self.depth_normalization[shape_index:shape_index + BOOK_LEVELS]
        if len(shape) < BOOK_LEVELS:
            raise ValueError(
                "spread selects an incomplete depth-normalization slice: "
                f"index={shape_index}, available={len(self.depth_normalization)}"
            )

        own_distribution = self._own_order_distribution(tranche, orders)
        level_codes, queue_codes = self._unit_codes(tranche, orders)

        observation = np.concatenate(
            [
                np.asarray(
                    [
                        tranche.normalized_time(now),
                        tranche.remaining / tranche.volume,
                        (market.best_bid - tranche.reference_bid) / price_scale,
                        (market.mid_price - tranche.reference_mid) / price_scale,
                        normalized_spread,
                        imbalance,
                    ],
                    dtype=np.float32,
                ),
                raw_bid / shape,
                raw_ask / shape,
                own_distribution,
                level_codes,
                queue_codes,
                np.asarray(
                    [
                        market.recent_market_imbalance,
                        market.recent_limit_imbalance,
                        market.recent_cancel_imbalance,
                        market.recent_mid_drift / price_scale,
                    ],
                    dtype=np.float32,
                ),
            ],
            dtype=np.float32,
        )

        if observation.shape != (OBSERVATION_DIMENSION,):
            raise RuntimeError(f"observation has unexpected shape {observation.shape}")
        if not np.isfinite(observation).all():
            raise ValueError("observation contains non-finite values")
        return observation

    @staticmethod
    def _own_order_distribution(
        tranche: Tranche,
        orders: TrancheOrderState,
    ) -> np.ndarray:
        remaining = tranche.remaining
        active_levels = list(orders.active_by_level)
        outside = orders.active_outside + orders.pending_aggressive
        inactive = max(remaining - sum(active_levels) - outside, 0)
        distribution = np.asarray(
            [*active_levels, outside, inactive],
            dtype=np.float32,
        ) / remaining
        total = float(distribution.sum())
        if total <= 0:
            distribution[-1] = 1.0
        elif not np.isclose(total, 1.0, atol=1e-5):
            distribution /= total
        return distribution

    @staticmethod
    def _unit_codes(
        tranche: Tranche,
        orders: TrancheOrderState,
    ) -> tuple[np.ndarray, np.ndarray]:
        levels: list[float] = []
        queues: list[float] = []

        represented = 0
        for level, quantity in enumerate(orders.active_by_level, start=1):
            quantity = min(quantity, tranche.remaining - represented)
            if quantity <= 0:
                continue
            queue_ahead = max(orders.queue_ahead_by_level[level - 1], 0.0)
            levels.extend([level / 6.0] * quantity)
            # RLTE encodes each lot's queue position separately and does not
            # clip positions above the training normalization constant of 40.
            queues.extend(
                (queue_ahead + unit_offset) / 40.0
                for unit_offset in range(quantity)
            )
            represented += quantity

        inactive = tranche.remaining - represented
        levels.extend([1.0] * inactive)
        queues.extend([1.0] * inactive)

        filled = tranche.volume - tranche.remaining
        levels.extend([-5.0 / 6.0] * filled)
        queues.extend([-1.0] * filled)

        if len(levels) != MODEL_INVENTORY or len(queues) != MODEL_INVENTORY:
            raise RuntimeError("unit encoding does not contain exactly 20 entries")
        return np.asarray(levels, dtype=np.float32), np.asarray(queues, dtype=np.float32)
