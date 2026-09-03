from __future__ import annotations

from dataclasses import dataclass

from vnpy.trader.constant import Exchange, Status
from vnpy.trader.object import CancelRequest, OrderData, OrderRequest

from vnpy_acrl_execution.ac_planner import AcPlanner
from vnpy_acrl_execution.coordinator import ExecutionCoordinator
from vnpy_acrl_execution.market_state import MarketSnapshot
from vnpy_acrl_execution.models import (
    AcParameters,
    ExecutionIntent,
    ParentOrder,
    ParentOrderRequest,
    ParentStatus,
)
from vnpy_acrl_execution.tranche_scheduler import TrancheScheduler


class FakeMainEngine:
    def __init__(self) -> None:
        self.sent: list[tuple[OrderRequest, str, str]] = []
        self.cancelled: list[tuple[CancelRequest, str]] = []

    def send_order(self, request: OrderRequest, gateway_name: str) -> str:
        vt_orderid = f"{gateway_name}.O{len(self.sent) + 1}"
        self.sent.append((request, gateway_name, vt_orderid))
        return vt_orderid

    def cancel_order(self, request: CancelRequest, gateway_name: str) -> None:
        self.cancelled.append((request, gateway_name))


def make_parent() -> tuple[ParentOrder, object]:
    request = ParentOrderRequest(
        symbol="TEST",
        exchange=Exchange.LOCAL,
        total_volume=20,
        duration_seconds=100,
        rl_horizon_seconds=100,
        max_active_tranches=1,
        price_tick=1,
        ac=AcParameters(volatility=0, risk_aversion=0),
    )
    plan = AcPlanner().create_plan(20, 100, request.ac)
    tranche = TrancheScheduler().build_tranches("P1", plan, 100)[0]
    tranche.activate(0, 0)
    parent = ParentOrder(
        parent_id="P1",
        request=request,
        created_at=0,
        started_at=0,
        tranches=[tranche],
    )
    return parent, tranche


def make_market() -> MarketSnapshot:
    return MarketSnapshot(
        timestamp=0,
        bid_prices=(100, 99, 98, 97, 96),
        ask_prices=(101, 102, 103, 104, 105),
        bid_depths=(50, 40, 30, 20, 10),
        ask_depths=(45, 35, 25, 15, 5),
        last_price=100,
        last_volume=1,
        recent_market_imbalance=0,
        recent_limit_imbalance=0,
        recent_cancel_imbalance=0,
        recent_mid_drift=0,
    )


def make_coordinator():
    main = FakeMainEngine()
    coordinator = ExecutionCoordinator(
        main_engine=main,
        on_parent_changed=lambda parent: None,
        on_tranche_changed=lambda tranche: None,
        write_log=lambda message: None,
    )
    parent, tranche = make_parent()
    coordinator.register_parent(parent)
    return main, coordinator, parent, tranche


def order_event(
    request: OrderRequest,
    vt_orderid: str,
    status: Status,
    traded: int,
) -> OrderData:
    orderid = vt_orderid.split(".", 1)[1]
    return OrderData(
        symbol=request.symbol,
        exchange=request.exchange,
        orderid=orderid,
        direction=request.direction,
        type=request.type,
        price=request.price,
        volume=request.volume,
        traded=traded,
        status=status,
        reference=request.reference,
        gateway_name="QUICKFIX",
    )


def test_repeated_target_does_not_repeat_new_order() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    intent = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])

    coordinator.apply_intent(parent, tranche, intent, make_market())
    coordinator.apply_intent(parent, tranche, intent, make_market())

    assert len(main.sent) == 1
    request, _, _ = main.sent[0]
    assert request.volume == 20
    assert request.price == 101
    coordinator.assert_hard_limits(parent)


def test_cancel_pending_does_not_release_inventory_early() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    level_one = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])
    level_two = ExecutionIntent.from_array([0, 0, 1, 0, 0, 0, 0])
    market = make_market()

    coordinator.apply_intent(parent, tranche, level_one, market)
    first_request, _, first_id = main.sent[0]
    coordinator.process_order(
        order_event(first_request, first_id, Status.NOTTRADED, traded=0)
    )

    coordinator.apply_intent(parent, tranche, level_two, market)

    assert len(main.cancelled) == 1
    assert len(main.sent) == 1
    coordinator.assert_hard_limits(parent)

    coordinator.process_order(
        order_event(first_request, first_id, Status.CANCELLED, traded=0)
    )
    coordinator.reconcile_pending(parent, tranche, market)

    assert len(main.sent) == 2
    second_request = main.sent[1][0]
    assert second_request.volume == 20
    assert second_request.price == 102


def test_partial_fill_updates_parent_and_reduces_next_order() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    level_one = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])
    level_two = ExecutionIntent.from_array([0, 0, 1, 0, 0, 0, 0])
    market = make_market()

    coordinator.apply_intent(parent, tranche, level_one, market)
    first_request, _, first_id = main.sent[0]
    coordinator.process_order(
        order_event(first_request, first_id, Status.PARTTRADED, traded=5)
    )

    assert parent.traded == 5
    assert tranche.traded == 5
    coordinator.apply_intent(parent, tranche, level_two, market)
    coordinator.process_order(
        order_event(first_request, first_id, Status.CANCELLED, traded=5)
    )
    coordinator.reconcile_pending(parent, tranche, market)

    assert main.sent[1][0].volume == 15
    coordinator.assert_hard_limits(parent)


def test_partial_fill_is_consumed_from_target_and_not_replenished() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    market_sell = ExecutionIntent.from_array([1, 0, 0, 0, 0, 0, 0])
    market = make_market()

    coordinator.apply_intent(parent, tranche, market_sell, market)
    request, _, vt_orderid = main.sent[0]
    coordinator.process_order(
        order_event(request, vt_orderid, Status.PARTTRADED, traded=5)
    )
    coordinator.reconcile_pending(parent, tranche, market)

    assert parent.traded == 5
    assert coordinator.targets[tranche.tranche_id].allocation.market_sell == 15
    assert len(main.sent) == 1
    coordinator.assert_hard_limits(parent)


def test_lower_aggressive_target_waits_for_cancel_then_sends_replacement() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    all_market = ExecutionIntent.from_array([1, 0, 0, 0, 0, 0, 0])
    quarter_market = ExecutionIntent.from_array([0.25, 0, 0, 0, 0, 0, 0.75])
    market = make_market()

    coordinator.apply_intent(parent, tranche, all_market, market)
    first_request, _, first_id = main.sent[0]
    coordinator.process_order(
        order_event(first_request, first_id, Status.NOTTRADED, traded=0)
    )

    coordinator.apply_intent(parent, tranche, quarter_market, market)

    assert len(main.cancelled) == 1
    assert len(main.sent) == 1
    coordinator.assert_hard_limits(parent)

    coordinator.process_order(
        order_event(first_request, first_id, Status.CANCELLED, traded=0)
    )
    coordinator.reconcile_pending(parent, tranche, market)

    assert len(main.sent) == 2
    replacement = main.sent[1][0]
    assert replacement.volume == 5
    assert replacement.price == market.best_bid
    coordinator.assert_hard_limits(parent)


def test_external_cancel_pauses_only_algorithm_parent() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    intent = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])
    coordinator.apply_intent(parent, tranche, intent, make_market())
    request, _, vt_orderid = main.sent[0]

    coordinator.process_order(
        order_event(request, vt_orderid, Status.CANCELLED, traded=0)
    )

    assert parent.status == ParentStatus.PAUSED
    assert "cancelled externally" in parent.message


def test_pause_cancels_children_without_sending_deadline_fallback() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    intent = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])
    market = make_market()
    coordinator.apply_intent(parent, tranche, intent, market)
    request, _, vt_orderid = main.sent[0]

    coordinator.pause_parent(parent, "test pause")
    assert len(main.cancelled) == 1
    coordinator.process_order(
        order_event(request, vt_orderid, Status.CANCELLED, traded=0)
    )

    # A pause is not a deadline.  The engine must not invoke close continuation
    # for a paused parent, and the coordinator itself has created no fallback.
    assert parent.status == ParentStatus.PAUSED
    assert len(main.sent) == 1


def test_deadline_cancels_existing_aggressive_order_before_fallback() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    market_sell = ExecutionIntent.from_array([1, 0, 0, 0, 0, 0, 0])
    market = make_market()
    coordinator.apply_intent(parent, tranche, market_sell, market)
    first_request, _, first_id = main.sent[0]
    coordinator.process_order(
        order_event(first_request, first_id, Status.NOTTRADED, traded=0)
    )

    coordinator.begin_tranche_close(parent, tranche, market)

    assert len(main.cancelled) == 1
    assert len(main.sent) == 1
    assert "waiting for cancel" in tranche.message

    coordinator.process_order(
        order_event(first_request, first_id, Status.CANCELLED, traded=0)
    )
    coordinator.continue_tranche_close(parent, tranche, market)

    assert len(main.sent) == 2
    fallback = main.sent[1][0]
    assert fallback.volume == 20
    assert fallback.price == market.best_bid
    coordinator.assert_hard_limits(parent)


def test_deadline_reanchors_fallback_when_best_bid_moves() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    initial = make_market()
    coordinator.begin_tranche_close(parent, tranche, initial)
    fallback, _, fallback_id = main.sent[0]
    coordinator.process_order(
        order_event(fallback, fallback_id, Status.NOTTRADED, traded=0)
    )
    moved = MarketSnapshot(
        timestamp=1,
        bid_prices=(99, 98, 97, 96, 95),
        ask_prices=(100, 101, 102, 103, 104),
        bid_depths=initial.bid_depths,
        ask_depths=initial.ask_depths,
        last_price=99,
        last_volume=1,
        recent_market_imbalance=0,
        recent_limit_imbalance=0,
        recent_cancel_imbalance=0,
        recent_mid_drift=-1,
    )

    coordinator.continue_tranche_close(parent, tranche, moved)

    assert len(main.cancelled) == 1
    assert len(main.sent) == 1
    coordinator.process_order(
        order_event(fallback, fallback_id, Status.CANCELLED, traded=0)
    )
    coordinator.continue_tranche_close(parent, tranche, moved)

    assert len(main.sent) == 2
    assert main.sent[1][0].price == moved.best_bid
    coordinator.assert_hard_limits(parent)


def test_observation_levels_are_anchored_at_best_ask() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    market = make_market()
    # With a one-tick spread, action level 1 and observation level 1 coincide.
    level_one = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])
    coordinator.apply_intent(parent, tranche, level_one, market)

    state = coordinator.order_state(parent, tranche, market)

    assert state.active_by_level == (20, 0, 0, 0, 0)
    assert state.queue_ahead_by_level[0] == market.ask_depths[0] - 20


def test_late_active_report_does_not_reopen_a_final_child_order() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    intent = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])
    coordinator.apply_intent(parent, tranche, intent, make_market())
    request, _, vt_orderid = main.sent[0]

    coordinator.process_order(
        order_event(request, vt_orderid, Status.ALLTRADED, traded=20)
    )
    coordinator.process_order(
        order_event(request, vt_orderid, Status.NOTTRADED, traded=0)
    )

    record = coordinator.child_orders[vt_orderid]
    assert record.status == Status.ALLTRADED
    assert record.leaves == 0
    assert parent.traded == 20


def test_late_different_final_status_does_not_pause_parent() -> None:
    main, coordinator, parent, tranche = make_coordinator()
    intent = ExecutionIntent.from_array([0, 1, 0, 0, 0, 0, 0])
    coordinator.apply_intent(parent, tranche, intent, make_market())
    request, _, vt_orderid = main.sent[0]

    coordinator.process_order(
        order_event(request, vt_orderid, Status.ALLTRADED, traded=20)
    )
    # Simulate an obsolete cancel terminal report arriving after the fill.
    coordinator.process_order(
        order_event(request, vt_orderid, Status.CANCELLED, traded=20)
    )

    assert coordinator.child_orders[vt_orderid].status == Status.ALLTRADED
    assert parent.status == ParentStatus.COMPLETED
