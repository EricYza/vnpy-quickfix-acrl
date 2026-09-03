from __future__ import annotations

from math import isclose

import numpy as np
from vnpy.trader.constant import Exchange

from vnpy_acrl_execution.ac_planner import AcPlanner
from vnpy_acrl_execution.market_state import MarketSnapshot
from vnpy_acrl_execution.models import (
    AcParameters,
    ParentOrder,
    ParentOrderRequest,
    TrancheOrderState,
)
from vnpy_acrl_execution.observation import ObservationBuilder
from vnpy_acrl_execution.tranche_scheduler import TrancheScheduler


def make_request(**overrides) -> ParentOrderRequest:
    values = {
        "symbol": "TEST",
        "exchange": Exchange.LOCAL,
        "total_volume": 260,
        "duration_seconds": 120,
        "rl_horizon_seconds": 30,
        "max_active_tranches": 4,
        "ac": AcParameters(
            volatility=0,
            temporary_impact=0.1,
            risk_aversion=0,
        ),
    }
    values.update(overrides)
    return ParentOrderRequest(**values)


def test_linear_ac_plan_and_inverse_milestones() -> None:
    request = make_request()
    plan = AcPlanner().create_plan(
        request.total_volume,
        request.duration_seconds,
        request.ac,
    )

    assert isclose(plan.cumulative_at(0), 0)
    assert isclose(plan.cumulative_at(60), 130)
    assert isclose(plan.cumulative_at(120), 260)
    assert isclose(plan.completion_time_for(20), 120 * 20 / 260)
    assert isclose(plan.completion_time_for(260), 120)


def test_tranches_are_twenty_lots_and_follow_ac_deadlines() -> None:
    request = make_request()
    plan = AcPlanner().create_plan(
        request.total_volume,
        request.duration_seconds,
        request.ac,
    )
    scheduler = TrancheScheduler()
    tranches = scheduler.build_tranches("P1", plan, request.rl_horizon_seconds)

    assert len(tranches) == 13
    assert all(tranche.volume == 20 for tranche in tranches)
    assert isclose(tranches[0].deadline_offset, 120 * 20 / 260)
    assert tranches[0].planned_start_offset == 0
    assert isclose(tranches[-1].deadline_offset, 120)
    assert isclose(tranches[-1].planned_start_offset, 90)

    capacity = scheduler.capacity(plan, request.rl_horizon_seconds, configured=4)
    assert capacity.required_concurrency == 4
    assert capacity.is_sufficient


def test_each_tranche_has_ten_decisions_on_its_own_clock() -> None:
    request = make_request(total_volume=20, duration_seconds=100)
    plan = AcPlanner().create_plan(20, 100, request.ac)
    tranche = TrancheScheduler().build_tranches("P1", plan, 100)[0]
    tranche.activate(now=1, parent_started_at=1)

    due_times: list[float] = []
    for now in range(1, 102):
        if tranche.decision_due(now):
            due_times.append(now)
            tranche.record_decision()

    assert due_times == [1, 11, 21, 31, 41, 51, 61, 71, 81, 91]
    assert tranche.decision_index == 10


def test_scheduler_starts_due_tranches_up_to_concurrency_limit() -> None:
    request = make_request(max_active_tranches=2)
    plan = AcPlanner().create_plan(260, 120, request.ac)
    scheduler = TrancheScheduler()
    tranches = scheduler.build_tranches("P1", plan, 30)
    parent = ParentOrder(
        parent_id="P1",
        request=request,
        created_at=0,
        started_at=0,
        tranches=tranches,
    )

    due = scheduler.due_to_start(parent, now=0)
    assert [tranche.sequence for tranche in due] == [1, 2]
    for tranche in due:
        tranche.activate(0, 0)
    assert scheduler.due_to_start(parent, now=20) == []


def test_observation_builder_produces_exact_67_features() -> None:
    request = make_request(total_volume=20, duration_seconds=100)
    plan = AcPlanner().create_plan(20, 100, request.ac)
    tranche = TrancheScheduler().build_tranches("P1", plan, 100)[0]
    tranche.activate(0, 0)
    tranche.traded = 5

    market = MarketSnapshot(
        timestamp=50,
        bid_prices=(100, 99, 98, 97, 96),
        ask_prices=(101, 102, 103, 104, 105),
        bid_depths=(20, 20, 20, 20, 20),
        ask_depths=(10, 10, 10, 10, 10),
        last_price=100,
        last_volume=1,
        recent_market_imbalance=0.1,
        recent_limit_imbalance=0.2,
        recent_cancel_imbalance=-0.3,
        recent_mid_drift=0.4,
    )
    orders = TrancheOrderState(
        active_by_level=(3, 2, 0, 0, 0),
        active_outside=0,
        pending_aggressive=0,
        queue_ahead_by_level=(4, 8, 0, 0, 0),
    )

    observation = ObservationBuilder.constant(20).build(
        tranche,
        market,
        orders,
        now=50,
        market_tick_size=1,
    )

    assert observation.shape == (67,)
    assert observation.dtype == np.float32
    assert isclose(float(observation[0]), 0.5)
    assert isclose(float(observation[1]), 0.75)
    assert np.isfinite(observation).all()
    assert len(observation[23:43]) == 20
    assert len(observation[43:63]) == 20


def test_queue_encoding_matches_rlte_per_lot_positions_without_clipping() -> None:
    request = make_request(total_volume=20, duration_seconds=100)
    plan = AcPlanner().create_plan(20, 100, request.ac)
    tranche = TrancheScheduler().build_tranches("P1", plan, 100)[0]
    tranche.activate(0, 0)
    orders = TrancheOrderState(
        active_by_level=(3, 0, 0, 0, 0),
        active_outside=0,
        pending_aggressive=0,
        queue_ahead_by_level=(50, 0, 0, 0, 0),
    )

    levels, queues = ObservationBuilder._unit_codes(tranche, orders)

    np.testing.assert_allclose(levels[:3], [1 / 6, 1 / 6, 1 / 6])
    np.testing.assert_allclose(queues[:3], [50 / 40, 51 / 40, 52 / 40])
    assert float(queues[0]) > 1.0


def test_market_snapshot_requires_all_five_ordered_price_levels() -> None:
    complete = MarketSnapshot(
        timestamp=0,
        bid_prices=(100, 99, 98, 97, 96),
        ask_prices=(101, 102, 103, 104, 105),
        bid_depths=(1, 1, 1, 1, 1),
        ask_depths=(1, 1, 1, 1, 1),
        last_price=100,
        last_volume=1,
        recent_market_imbalance=0,
        recent_limit_imbalance=0,
        recent_cancel_imbalance=0,
        recent_mid_drift=0,
    )
    incomplete = MarketSnapshot(
        timestamp=0,
        bid_prices=(100, 0, 0, 0, 0),
        ask_prices=(101, 0, 0, 0, 0),
        bid_depths=(1, 0, 0, 0, 0),
        ask_depths=(1, 0, 0, 0, 0),
        last_price=100,
        last_volume=1,
        recent_market_imbalance=0,
        recent_limit_imbalance=0,
        recent_cancel_imbalance=0,
        recent_mid_drift=0,
    )

    assert complete.has_complete_five_level_book
    assert not incomplete.has_complete_five_level_book
