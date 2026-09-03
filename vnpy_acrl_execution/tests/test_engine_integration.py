from __future__ import annotations

from vnpy.event import Event
from vnpy.trader.constant import Exchange, Status
from vnpy.trader.object import CancelRequest, OrderData, OrderRequest

from vnpy_acrl_execution.engine import AcRlExecutionEngine
from vnpy_acrl_execution.models import (
    AcParameters,
    ParentOrderRequest,
    ParentStatus,
)


class FakeEventEngine:
    def __init__(self) -> None:
        self.handlers: dict[str, list] = {}
        self.events: list[Event] = []

    def register(self, event_type: str, handler) -> None:
        self.handlers.setdefault(event_type, []).append(handler)

    def unregister(self, event_type: str, handler) -> None:
        handlers = self.handlers.get(event_type, [])
        if handler in handlers:
            handlers.remove(handler)

    def put(self, event: Event) -> None:
        self.events.append(event)


class LoggedOnGateway:
    is_logged_on = True


class FakeMainEngine:
    def __init__(self) -> None:
        self.gateway = LoggedOnGateway()
        self.sent: list[tuple[OrderRequest, str, str]] = []
        self.cancelled: list[tuple[CancelRequest, str]] = []
        self.logs: list[tuple[str, str]] = []

    def get_gateway(self, gateway_name: str):
        return self.gateway if gateway_name == "QUICKFIX" else None

    def send_order(self, request: OrderRequest, gateway_name: str) -> str:
        vt_orderid = f"{gateway_name}.E{len(self.sent) + 1}"
        self.sent.append((request, gateway_name, vt_orderid))
        return vt_orderid

    def cancel_order(self, request: CancelRequest, gateway_name: str) -> None:
        self.cancelled.append((request, gateway_name))

    def subscribe(self, request, gateway_name: str) -> None:
        raise AssertionError("synthetic mode must not subscribe through the gateway")

    def write_log(self, message: str, source: str) -> None:
        self.logs.append((message, source))


class FailingPolicy:
    def predict(self, observation):
        raise RuntimeError("intentional inference failure")


class NoopPolicy:
    def predict(self, observation):
        raise AssertionError("policy inference is not expected in this test")


def fill_event(request: OrderRequest, vt_orderid: str) -> Event:
    orderid = vt_orderid.split(".", 1)[1]
    return Event(
        "eOrder",
        OrderData(
            symbol=request.symbol,
            exchange=request.exchange,
            orderid=orderid,
            direction=request.direction,
            type=request.type,
            offset=request.offset,
            price=request.price,
            volume=request.volume,
            traded=request.volume,
            status=Status.ALLTRADED,
            reference=request.reference,
            gateway_name="QUICKFIX",
        ),
    )


def test_synthetic_parent_runs_through_policy_orders_and_fills(monkeypatch) -> None:
    clock = {"now": 0.0}
    monkeypatch.setattr(
        "vnpy_acrl_execution.engine.monotonic",
        lambda: clock["now"],
    )
    event_engine = FakeEventEngine()
    main_engine = FakeMainEngine()
    engine = AcRlExecutionEngine(main_engine, event_engine)
    request = ParentOrderRequest(
        symbol="DRYRUN",
        exchange=Exchange.LOCAL,
        total_volume=20,
        duration_seconds=10,
        rl_horizon_seconds=10,
        max_active_tranches=1,
        price_tick=0.01,
        use_synthetic_market=True,
        ac=AcParameters(volatility=0, risk_aversion=0),
    )
    parent_id = engine.start_parent(request, use_rule_policy=True, now=0)
    consumed = 0

    for second in range(10):
        clock["now"] = float(second)
        engine.process_timer_event(Event("eTimer"))
        while consumed < len(main_engine.sent):
            child_request, _, vt_orderid = main_engine.sent[consumed]
            consumed += 1
            assert child_request.reference.startswith(f"ACRL:{parent_id}:")
            engine.process_order_event(fill_event(child_request, vt_orderid))
        if engine.parents[parent_id].status == ParentStatus.COMPLETED:
            break

    parent = engine.parents[parent_id]
    assert parent.status == ParentStatus.COMPLETED
    assert parent.traded == 20
    assert sum(int(request.volume) for request, _, _ in main_engine.sent) == 20
    assert not main_engine.cancelled
    engine.coordinator.assert_hard_limits(parent)
    engine.close()


def test_parent_pauses_before_decision_when_fix_session_logs_out(monkeypatch) -> None:
    monkeypatch.setattr("vnpy_acrl_execution.engine.monotonic", lambda: 0.0)
    event_engine = FakeEventEngine()
    main_engine = FakeMainEngine()
    engine = AcRlExecutionEngine(main_engine, event_engine)
    request = ParentOrderRequest(
        symbol="LOGOUT",
        exchange=Exchange.LOCAL,
        total_volume=20,
        duration_seconds=10,
        rl_horizon_seconds=10,
        max_active_tranches=1,
        use_synthetic_market=True,
        ac=AcParameters(volatility=0, risk_aversion=0),
    )
    parent_id = engine.start_parent(request, use_rule_policy=True, now=0)
    main_engine.gateway.is_logged_on = False

    engine.process_timer_event(Event("eTimer"))

    parent = engine.parents[parent_id]
    assert parent.status == ParentStatus.PAUSED
    assert "not logged on" in parent.message
    assert not main_engine.sent
    engine.close()


def test_policy_exception_pauses_only_the_parent_and_does_not_escape(monkeypatch) -> None:
    monkeypatch.setattr("vnpy_acrl_execution.engine.monotonic", lambda: 0.0)
    event_engine = FakeEventEngine()
    main_engine = FakeMainEngine()
    engine = AcRlExecutionEngine(main_engine, event_engine)
    request = ParentOrderRequest(
        symbol="BADPOLICY",
        exchange=Exchange.LOCAL,
        total_volume=20,
        duration_seconds=10,
        rl_horizon_seconds=10,
        max_active_tranches=1,
        use_synthetic_market=True,
        ac=AcParameters(volatility=0, risk_aversion=0),
    )
    parent_id = engine.start_parent(request, use_rule_policy=True, now=0)
    engine.parent_policies[parent_id] = FailingPolicy()

    engine.process_timer_event(Event("eTimer"))

    parent = engine.parents[parent_id]
    assert parent.status == ParentStatus.PAUSED
    assert "intentional inference failure" in parent.message
    assert not main_engine.sent
    assert any("intentional inference failure" in message for message, _ in main_engine.logs)
    engine.close()


def test_parent_clock_starts_after_policy_loading(monkeypatch) -> None:
    clock = {"now": 100.0}
    monkeypatch.setattr(
        "vnpy_acrl_execution.engine.monotonic",
        lambda: clock["now"],
    )
    event_engine = FakeEventEngine()
    main_engine = FakeMainEngine()
    engine = AcRlExecutionEngine(main_engine, event_engine)

    def load_policy(*_args):
        clock["now"] = 105.0
        return NoopPolicy()

    monkeypatch.setattr(engine, "_load_policy", load_policy)
    request = ParentOrderRequest(
        symbol="LOADCLOCK",
        exchange=Exchange.LOCAL,
        total_volume=20,
        duration_seconds=10,
        rl_horizon_seconds=10,
        max_active_tranches=1,
        use_synthetic_market=True,
        ac=AcParameters(volatility=0, risk_aversion=0),
    )

    parent_id = engine.start_parent(request)

    parent = engine.parents[parent_id]
    assert parent.started_at == 105.0
    assert parent.deadline_at == 115.0
    engine.close()
