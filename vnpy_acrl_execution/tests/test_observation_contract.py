from __future__ import annotations

import bisect
import sys
from pathlib import Path

import numpy as np

from vnpy_acrl_execution.ac_planner import AcPlanner
from vnpy_acrl_execution.constants import (
    DEFAULT_BOOK_SHAPE_PATH,
    DEFAULT_RLTE_ROOT,
    EXPECTED_ACTION_NAMES,
    EXPECTED_FEATURE_NAMES,
    MODEL_CONTRACT_VERSION,
)
from vnpy_acrl_execution.market_state import MarketSnapshot
from vnpy_acrl_execution.models import (
    AcParameters,
    ParentOrderRequest,
    TrancheOrderState,
)
from vnpy_acrl_execution.observation import ObservationBuilder
from vnpy_acrl_execution.tranche_scheduler import TrancheScheduler
from vnpy.trader.constant import Exchange


def _imbalance(first: list[float], second: list[float]) -> float:
    total = np.sum(first) + np.sum(second)
    return float((np.sum(first) - np.sum(second)) / total) if total else 0.0


def _reference_state():
    """Create one state and ask RLTE's original RLAgent for the reference vector."""

    root = Path(DEFAULT_RLTE_ROOT)
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

    from limit_order_book.limit_order_book import LimitOrder, LimitOrderBook
    from simulation.agents import RLAgent

    lob = LimitOrderBook(list_of_agents=["noise", "rl_agent"], level=30)
    bid_depths = (12, 14, 16, 18, 20)
    ask_depths = (11, 13, 15, 17, 19)
    for level, volume in enumerate(bid_depths):
        lob.process_order(LimitOrder("noise", "bid", 1000 - level, volume, 0))
    for level, volume in enumerate(ask_depths):
        lob.process_order(LimitOrder("noise", "ask", 1001 + level, volume, 0))

    agent = RLAgent(
        action_book_levels=5,
        observation_book_levels=5,
        volume=20,
        terminal_time=150,
        start_time=0,
        time_delta=15,
        priority=0,
        initial_shape_file=str(DEFAULT_BOOK_SHAPE_PATH),
    )
    agent.get_observation(time=0, lob=lob)

    # Two own orders are behind all pre-existing volume at their price levels.
    lob.process_order(LimitOrder("rl_agent", "ask", 1001, 2, 15))
    lob.process_order(LimitOrder("rl_agent", "ask", 1002, 3, 15))
    agent.active_volume = 5
    agent.volume = 17
    expected = agent.get_observation(time=15, lob=lob)

    start = bisect.bisect_left(lob.data.time_stamps, 0)
    market_buys = [
        value
        for value, timestamp in zip(lob.data.market_buy, lob.data.time_stamps)
        if 0 <= timestamp <= 15
    ]
    market_sells = [
        value
        for value, timestamp in zip(lob.data.market_sell, lob.data.time_stamps)
        if 0 <= timestamp <= 15
    ]
    limit_buys = [
        value
        for value, timestamp in zip(lob.data.limit_buy, lob.data.time_stamps)
        if 0 <= timestamp <= 15
    ]
    limit_sells = [
        value
        for value, timestamp in zip(lob.data.limit_sell, lob.data.time_stamps)
        if 0 <= timestamp <= 15
    ]
    cancel_sells = [
        value
        for value, timestamp in zip(
            lob.data.cancellation_limit_sell,
            lob.data.time_stamps,
        )
        if 0 <= timestamp <= 15
    ]
    cancel_buys = [
        value
        for value, timestamp in zip(
            lob.data.cancellation_limit_buy,
            lob.data.time_stamps,
        )
        if 0 <= timestamp <= 15
    ]
    old_mid = (
        lob.data.best_bid_prices[start] + lob.data.best_ask_prices[start]
    ) / 2
    current_mid = (
        lob.data.best_bid_prices[-1] + lob.data.best_ask_prices[-1]
    ) / 2
    snapshot = MarketSnapshot(
        timestamp=15,
        bid_prices=tuple(float(value) for value in lob.data.bid_prices[-1][:5]),
        ask_prices=tuple(float(value) for value in lob.data.ask_prices[-1][:5]),
        bid_depths=tuple(float(value) for value in lob.data.bid_volumes[-1][:5]),
        ask_depths=tuple(float(value) for value in lob.data.ask_volumes[-1][:5]),
        last_price=1001,
        last_volume=0,
        recent_market_imbalance=_imbalance(market_buys, market_sells),
        recent_limit_imbalance=_imbalance(limit_buys, limit_sells),
        recent_cancel_imbalance=_imbalance(cancel_sells, cancel_buys),
        recent_mid_drift=current_mid - old_mid,
    )
    order_state = TrancheOrderState(
        active_by_level=(2, 3, 0, 0, 0),
        active_outside=0,
        pending_aggressive=0,
        queue_ahead_by_level=(11, 13, 0, 0, 0),
    )
    return agent, expected, snapshot, order_state


def _tranche():
    request = ParentOrderRequest(
        symbol="CONTRACT",
        exchange=Exchange.LOCAL,
        total_volume=20,
        duration_seconds=150,
        rl_horizon_seconds=150,
        max_active_tranches=1,
        price_tick=1,
        ac=AcParameters(volatility=0, risk_aversion=0),
    )
    plan = AcPlanner().create_plan(20, 150, request.ac)
    tranche = TrancheScheduler().build_tranches("P1", plan, 150)[0]
    tranche.activate(0, 0)
    tranche.traded = 3
    return tranche


def test_adapter_matches_original_rlte_observation_element_by_element() -> None:
    agent, expected, snapshot, order_state = _reference_state()
    tranche = _tranche()
    tranche.reference_bid = float(agent.reference_bid_price)
    tranche.reference_mid = float(agent.reference_mid_price)

    actual = ObservationBuilder.from_shape_file(DEFAULT_BOOK_SHAPE_PATH).build(
        tranche,
        snapshot,
        order_state,
        now=15,
        market_tick_size=1,
    )

    assert expected.shape == actual.shape == (67,)
    np.testing.assert_array_equal(actual, expected)


def test_tick_size_is_only_a_unit_adapter_not_a_new_feature() -> None:
    _, integer_ticks, snapshot, order_state = _reference_state()
    tranche = _tranche()
    tranche.reference_bid = 10.0
    tranche.reference_mid = 10.005
    scaled = MarketSnapshot(
        timestamp=snapshot.timestamp,
        bid_prices=tuple(value / 100 for value in snapshot.bid_prices),
        ask_prices=tuple(value / 100 for value in snapshot.ask_prices),
        bid_depths=snapshot.bid_depths,
        ask_depths=snapshot.ask_depths,
        last_price=snapshot.last_price / 100,
        last_volume=snapshot.last_volume,
        recent_market_imbalance=snapshot.recent_market_imbalance,
        recent_limit_imbalance=snapshot.recent_limit_imbalance,
        recent_cancel_imbalance=snapshot.recent_cancel_imbalance,
        recent_mid_drift=snapshot.recent_mid_drift / 100,
    )

    actual = ObservationBuilder.from_shape_file(DEFAULT_BOOK_SHAPE_PATH).build(
        tranche,
        scaled,
        order_state,
        now=15,
        market_tick_size=0.01,
    )

    np.testing.assert_allclose(actual, integer_ticks, rtol=0, atol=1e-6)

    schemas_path = Path(DEFAULT_RLTE_ROOT) / "deployment" / "schemas.py"
    assert schemas_path.is_file()
    if str(DEFAULT_RLTE_ROOT) not in sys.path:
        sys.path.insert(0, str(DEFAULT_RLTE_ROOT))
    from deployment.schemas import ACTION_NAMES, CONTRACT_VERSION, FEATURE_NAMES

    assert CONTRACT_VERSION == MODEL_CONTRACT_VERSION
    assert tuple(FEATURE_NAMES) == EXPECTED_FEATURE_NAMES
    assert tuple(ACTION_NAMES) == EXPECTED_ACTION_NAMES
    assert "price_tick" not in FEATURE_NAMES
    assert "market_tick_size" not in FEATURE_NAMES


def test_wide_spread_continuous_tick_grid_matches_original_rlte() -> None:
    root = Path(DEFAULT_RLTE_ROOT)
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from limit_order_book.limit_order_book import LimitOrder, LimitOrderBook
    from simulation.agents import RLAgent

    lob = LimitOrderBook(list_of_agents=["noise", "rl_agent"], level=30)
    bid_prices = (1000, 999, 998, 997, 996)
    ask_prices = (1003, 1004, 1005, 1006, 1007)
    bid_depths = (12, 14, 16, 18, 20)
    ask_depths = (11, 13, 15, 17, 19)
    for price, volume in zip(bid_prices, bid_depths):
        lob.process_order(LimitOrder("noise", "bid", price, volume, 0))
    for price, volume in zip(ask_prices, ask_depths):
        lob.process_order(LimitOrder("noise", "ask", price, volume, 0))

    agent = RLAgent(
        action_book_levels=5,
        observation_book_levels=5,
        volume=20,
        terminal_time=150,
        start_time=0,
        time_delta=15,
        priority=0,
        initial_shape_file=str(DEFAULT_BOOK_SHAPE_PATH),
    )
    expected = agent.get_observation(time=0, lob=lob)
    snapshot = MarketSnapshot(
        timestamp=0,
        bid_prices=tuple(float(value) for value in bid_prices),
        ask_prices=tuple(float(value) for value in ask_prices),
        bid_depths=tuple(float(value) for value in bid_depths),
        ask_depths=tuple(float(value) for value in ask_depths),
        last_price=1001.5,
        last_volume=0,
        recent_market_imbalance=0,
        recent_limit_imbalance=_imbalance(list(bid_depths), list(ask_depths)),
        recent_cancel_imbalance=0,
        recent_mid_drift=0,
    )
    order_state = TrancheOrderState(
        active_by_level=(0, 0, 0, 0, 0),
        active_outside=0,
        pending_aggressive=0,
        queue_ahead_by_level=(0, 0, 0, 0, 0),
    )
    tranche = _tranche()
    tranche.traded = 0

    actual = ObservationBuilder.from_shape_file(DEFAULT_BOOK_SHAPE_PATH).build(
        tranche,
        snapshot,
        order_state,
        now=0,
        market_tick_size=1,
    )

    np.testing.assert_array_equal(actual, expected)
    model_bid, model_ask = snapshot.model_book_depths(1)
    assert model_bid == (0, 0, 12, 14, 16)
    assert model_ask == (0, 0, 11, 13, 15)
