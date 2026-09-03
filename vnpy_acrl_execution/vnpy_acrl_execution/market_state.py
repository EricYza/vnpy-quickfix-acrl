from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from math import isclose
from threading import RLock
from time import monotonic

from vnpy.trader.object import TickData

from .constants import BOOK_LEVELS


def _ratio(first: float, second: float) -> float:
    total = first + second
    return (first - second) / total if total else 0.0


@dataclass(frozen=True)
class FlowEvent:
    timestamp: float
    kind: str
    side: str
    volume: float


@dataclass(frozen=True)
class MarketSnapshot:
    timestamp: float
    bid_prices: tuple[float, float, float, float, float]
    ask_prices: tuple[float, float, float, float, float]
    bid_depths: tuple[float, float, float, float, float]
    ask_depths: tuple[float, float, float, float, float]
    last_price: float
    last_volume: float
    recent_market_imbalance: float
    recent_limit_imbalance: float
    recent_cancel_imbalance: float
    recent_mid_drift: float

    @property
    def best_bid(self) -> float:
        return self.bid_prices[0]

    @property
    def best_ask(self) -> float:
        return self.ask_prices[0]

    @property
    def mid_price(self) -> float:
        return (self.best_bid + self.best_ask) / 2.0

    @property
    def has_complete_five_level_book(self) -> bool:
        """Return whether all model-required price levels are present and ordered."""

        if not all(price > 0 for price in (*self.bid_prices, *self.ask_prices)):
            return False
        if not all(depth >= 0 for depth in (*self.bid_depths, *self.ask_depths)):
            return False
        if any(
            better <= worse
            for better, worse in zip(self.bid_prices, self.bid_prices[1:])
        ):
            return False
        if any(
            better >= worse
            for better, worse in zip(self.ask_prices, self.ask_prices[1:])
        ):
            return False
        return self.best_ask > self.best_bid

    def model_book_depths(
        self,
        market_tick_size: float,
    ) -> tuple[tuple[float, ...], tuple[float, ...]]:
        """Project ranked quotes onto RLTE's continuous five-tick depth grid.

        RLTE ``LimitOrderBook.level2`` starts bid depth at ``best_ask - 1``
        tick and ask depth at ``best_bid + 1`` tick.  Empty prices inside a
        spread are represented by zero depth.  FIX/vn.py usually expose ranked
        non-empty levels, so this projection is required before constructing
        the unchanged 67-feature vector.
        """

        if market_tick_size <= 0:
            raise ValueError("market_tick_size must be positive")
        if not self.has_complete_five_level_book:
            raise ValueError("a complete ordered five-level book is required")

        bid_targets = tuple(
            self.best_ask - market_tick_size * level
            for level in range(1, BOOK_LEVELS + 1)
        )
        ask_targets = tuple(
            self.best_bid + market_tick_size * level
            for level in range(1, BOOK_LEVELS + 1)
        )
        return (
            tuple(
                self.depth_at_price("bid", price, market_tick_size)
                for price in bid_targets
            ),
            tuple(
                self.depth_at_price("ask", price, market_tick_size)
                for price in ask_targets
            ),
        )

    def depth_at_price(
        self,
        side: str,
        target_price: float,
        market_tick_size: float,
    ) -> float:
        """Return ranked-book depth at one tick-aligned price, or zero."""

        if side == "bid":
            prices = self.bid_prices
            depths = self.bid_depths
        elif side == "ask":
            prices = self.ask_prices
            depths = self.ask_depths
        else:
            raise ValueError(f"unsupported book side: {side}")
        if market_tick_size <= 0:
            raise ValueError("market_tick_size must be positive")

        tolerance = market_tick_size * 1e-6
        for price, depth in zip(prices, depths):
            if isclose(price, target_price, rel_tol=0.0, abs_tol=tolerance):
                return depth
        return 0.0


class MarketStateCache:
    def __init__(self, retention_seconds: float = 3600.0) -> None:
        self._retention_seconds = retention_seconds
        self._lock = RLock()
        self._ticks: dict[str, tuple[float, TickData]] = {}
        self._flows: dict[str, deque[FlowEvent]] = {}
        self._mid_history: dict[str, deque[tuple[float, float]]] = {}

    def update_tick(self, tick: TickData, timestamp: float | None = None) -> None:
        now = monotonic() if timestamp is None else timestamp
        with self._lock:
            previous_entry = self._ticks.get(tick.vt_symbol)
            self._ticks[tick.vt_symbol] = (now, tick)
            flows = self._flows.setdefault(tick.vt_symbol, deque())
            mids = self._mid_history.setdefault(tick.vt_symbol, deque())

            bid = tick.bid_price_1
            ask = tick.ask_price_1
            if bid and ask:
                mids.append((now, (bid + ask) / 2.0))

            if previous_entry:
                _, previous = previous_entry
                self._infer_depth_flow(flows, previous, tick, now)

            if tick.last_volume > 0 and bid and ask:
                if tick.last_price >= ask:
                    flows.append(FlowEvent(now, "market", "buy", tick.last_volume))
                elif tick.last_price <= bid:
                    flows.append(FlowEvent(now, "market", "sell", tick.last_volume))

            self._purge(flows, mids, now)

    def record_flow(
        self,
        vt_symbol: str,
        kind: str,
        side: str,
        volume: float,
        timestamp: float | None = None,
    ) -> None:
        if kind not in {"market", "limit", "cancel"}:
            raise ValueError(f"unsupported flow kind: {kind}")
        if side not in {"buy", "sell"}:
            raise ValueError(f"unsupported flow side: {side}")
        now = monotonic() if timestamp is None else timestamp
        with self._lock:
            flows = self._flows.setdefault(vt_symbol, deque())
            flows.append(FlowEvent(now, kind, side, volume))

    def snapshot(
        self,
        vt_symbol: str,
        window_seconds: float,
        now: float | None = None,
    ) -> MarketSnapshot | None:
        current = monotonic() if now is None else now
        with self._lock:
            entry = self._ticks.get(vt_symbol)
            if not entry:
                return None
            timestamp, tick = entry
            flows = self._flows.get(vt_symbol, deque())
            window_start = current - window_seconds

            totals = {
                ("market", "buy"): 0.0,
                ("market", "sell"): 0.0,
                ("limit", "buy"): 0.0,
                ("limit", "sell"): 0.0,
                ("cancel", "buy"): 0.0,
                ("cancel", "sell"): 0.0,
            }
            for event in flows:
                if event.timestamp >= window_start:
                    totals[(event.kind, event.side)] += event.volume

            mids = self._mid_history.get(vt_symbol, deque())
            current_mid = (tick.bid_price_1 + tick.ask_price_1) / 2.0
            old_mid = current_mid
            for mid_time, value in mids:
                if mid_time >= window_start:
                    old_mid = value
                    break

            return MarketSnapshot(
                timestamp=timestamp,
                bid_prices=self._prices(tick, "bid"),
                ask_prices=self._prices(tick, "ask"),
                bid_depths=self._depths(tick, "bid"),
                ask_depths=self._depths(tick, "ask"),
                last_price=tick.last_price,
                last_volume=tick.last_volume,
                recent_market_imbalance=_ratio(
                    totals[("market", "buy")], totals[("market", "sell")]
                ),
                recent_limit_imbalance=_ratio(
                    totals[("limit", "buy")], totals[("limit", "sell")]
                ),
                recent_cancel_imbalance=_ratio(
                    totals[("cancel", "sell")], totals[("cancel", "buy")]
                ),
                recent_mid_drift=current_mid - old_mid,
            )

    def _infer_depth_flow(
        self,
        flows: deque[FlowEvent],
        previous: TickData,
        current: TickData,
        now: float,
    ) -> None:
        for side in ("bid", "ask"):
            flow_side = "buy" if side == "bid" else "sell"
            old_total = sum(self._depths(previous, side))
            new_total = sum(self._depths(current, side))
            difference = new_total - old_total
            if difference > 0:
                flows.append(FlowEvent(now, "limit", flow_side, difference))
            elif difference < 0:
                flows.append(FlowEvent(now, "cancel", flow_side, -difference))

    def _purge(
        self,
        flows: deque[FlowEvent],
        mids: deque[tuple[float, float]],
        now: float,
    ) -> None:
        cutoff = now - self._retention_seconds
        while flows and flows[0].timestamp < cutoff:
            flows.popleft()
        while mids and mids[0][0] < cutoff:
            mids.popleft()

    @staticmethod
    def _prices(tick: TickData, side: str) -> tuple[float, float, float, float, float]:
        values = [float(getattr(tick, f"{side}_price_{level}")) for level in range(1, BOOK_LEVELS + 1)]
        return tuple(values)

    @staticmethod
    def _depths(tick: TickData, side: str) -> tuple[float, float, float, float, float]:
        values = [float(getattr(tick, f"{side}_volume_{level}")) for level in range(1, BOOK_LEVELS + 1)]
        return tuple(values)
