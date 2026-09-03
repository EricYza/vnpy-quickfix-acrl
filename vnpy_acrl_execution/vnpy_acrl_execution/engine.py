from __future__ import annotations

from datetime import UTC, datetime
from pathlib import Path
from threading import RLock
from time import monotonic

from vnpy.event import EVENT_TIMER, Event, EventEngine
from vnpy.trader.engine import BaseEngine, MainEngine
from vnpy.trader.event import EVENT_ORDER, EVENT_TICK, EVENT_TRADE
from vnpy.trader.object import OrderData, SubscribeRequest, TickData

from .ac_planner import AcPlan, AcPlanner
from .constants import (
    APP_NAME,
    DEFAULT_BOOK_SHAPE_PATH,
    DEFAULT_MODEL_PATH,
    DEFAULT_MODEL_SHA256,
    DEFAULT_RLTE_ROOT,
    EVENT_ACRL_INTENT,
    EVENT_ACRL_PARENT,
    EVENT_ACRL_TRANCHE,
)
from .coordinator import ExecutionCoordinator
from .market_state import MarketSnapshot, MarketStateCache
from .models import (
    IntentRecord,
    ParentOrder,
    ParentOrderRequest,
    ParentStatus,
    Tranche,
    TrancheStatus,
)
from .observation import ObservationBuilder
from .policy_adapter import ExecutionPolicy, RltePolicyAdapter, RuleBasedPolicy
from .synthetic_market import SyntheticMarketFeed
from .tranche_scheduler import ScheduleCapacity, TrancheScheduler


class AcRlExecutionEngine(BaseEngine):
    def __init__(self, main_engine: MainEngine, event_engine: EventEngine) -> None:
        super().__init__(main_engine, event_engine, APP_NAME)
        self.planner = AcPlanner()
        self.scheduler = TrancheScheduler()
        self.market_cache = MarketStateCache()
        self.synthetic_feed = SyntheticMarketFeed()
        self.parents: dict[str, ParentOrder] = {}
        self.plans: dict[str, AcPlan] = {}
        self.parent_policies: dict[str, ExecutionPolicy] = {}
        self._policy_cache: dict[tuple[Path, Path], RltePolicyAdapter] = {}
        self._parent_counter = 0
        self._lock = RLock()
        self._missing_market_logged: set[str] = set()

        if DEFAULT_BOOK_SHAPE_PATH.is_file():
            self.observation_builder = ObservationBuilder.from_shape_file(
                DEFAULT_BOOK_SHAPE_PATH
            )
        else:
            self.observation_builder = ObservationBuilder.constant()
            self.write_log(
                f"Book-shape file missing; using constant normalization: "
                f"{DEFAULT_BOOK_SHAPE_PATH}"
            )

        self.coordinator = ExecutionCoordinator(
            main_engine=main_engine,
            on_parent_changed=self.emit_parent,
            on_tranche_changed=self.emit_tranche,
            write_log=self.write_log,
        )
        self.event_engine.register(EVENT_TIMER, self.process_timer_event)
        self.event_engine.register(EVENT_TICK, self.process_tick_event)
        self.event_engine.register(EVENT_ORDER, self.process_order_event)
        self.event_engine.register(EVENT_TRADE, self.process_trade_event)

    def start_parent(
        self,
        request: ParentOrderRequest,
        *,
        rlte_root: str | Path = DEFAULT_RLTE_ROOT,
        model_path: str | Path = DEFAULT_MODEL_PATH,
        use_rule_policy: bool = False,
        now: float | None = None,
    ) -> str:
        with self._lock:
            gateway = self.main_engine.get_gateway(request.gateway_name)
            if gateway is None:
                raise RuntimeError(f"Gateway is not available: {request.gateway_name}")
            if getattr(gateway, "is_logged_on", None) is not True:
                raise RuntimeError(
                    f"Gateway {request.gateway_name} has not completed FIX Logon"
                )

            plan = self.planner.create_plan(
                request.total_volume,
                request.duration_seconds,
                request.ac,
            )
            capacity = self.scheduler.capacity(
                plan,
                request.rl_horizon_seconds,
                request.max_active_tranches,
            )
            if not capacity.is_sufficient:
                raise ValueError(
                    "AC curve needs at least "
                    f"{capacity.required_concurrency} active 20-lot tranches, "
                    f"but max_active_tranches={capacity.configured_concurrency}"
                )

            policy: ExecutionPolicy
            if use_rule_policy:
                policy = RuleBasedPolicy()
            else:
                policy = self._load_policy(rlte_root, model_path)

            # Model loading can take noticeable time on the first request.  A
            # live parent starts only after inference is ready, so that delay
            # does not silently consume part of its AC/RL execution horizon.
            current = monotonic() if now is None else now

            self._parent_counter += 1
            stamp = datetime.now(UTC).strftime("%Y%m%d%H%M%S%f")
            parent_id = f"ACRL{stamp}-{self._parent_counter:03d}"
            tranches = self.scheduler.build_tranches(
                parent_id,
                plan,
                request.rl_horizon_seconds,
            )
            parent = ParentOrder(
                parent_id=parent_id,
                request=request,
                created_at=current,
                started_at=current,
                tranches=tranches,
            )
            self.parents[parent_id] = parent
            self.plans[parent_id] = plan
            self.parent_policies[parent_id] = policy
            self.coordinator.register_parent(parent)

            if not request.use_synthetic_market:
                subscription = SubscribeRequest(
                    symbol=request.symbol,
                    exchange=request.exchange,
                )
                self.main_engine.subscribe(subscription, request.gateway_name)

            self.write_log(
                f"Parent started id={parent_id} symbol={request.symbol} "
                f"volume={request.total_volume} duration={request.duration_seconds}s "
                f"tranches={len(tranches)} required_concurrency="
                f"{capacity.required_concurrency} policy="
                f"{'RULE_TEST' if use_rule_policy else 'RLTE'} market="
                f"{'SYNTHETIC' if request.use_synthetic_market else 'GATEWAY'}"
            )
            self.emit_parent(parent)
            for tranche in tranches:
                self.emit_tranche(tranche)
            return parent_id

    def estimate_capacity(self, request: ParentOrderRequest) -> ScheduleCapacity:
        plan = self.planner.create_plan(
            request.total_volume,
            request.duration_seconds,
            request.ac,
        )
        return self.scheduler.capacity(
            plan,
            request.rl_horizon_seconds,
            request.max_active_tranches,
        )

    def pause_parent(self, parent_id: str) -> None:
        parent = self._get_parent(parent_id)
        self.coordinator.pause_parent(parent, "Paused by user")

    def resume_parent(self, parent_id: str) -> None:
        parent = self._get_parent(parent_id)
        if monotonic() >= parent.deadline_at:
            raise RuntimeError("parent deadline has passed and cannot be resumed")
        self.coordinator.resume_parent(parent)

    def cancel_parent(self, parent_id: str) -> None:
        parent = self._get_parent(parent_id)
        self.coordinator.cancel_parent(parent)

    def get_all_parents(self) -> list[ParentOrder]:
        return list(self.parents.values())

    def process_tick_event(self, event: Event) -> None:
        tick: TickData = event.data
        self.market_cache.update_tick(tick)
        self._missing_market_logged.discard(tick.vt_symbol)

    def process_order_event(self, event: Event) -> None:
        order: OrderData = event.data
        record = self.coordinator.child_orders.get(order.vt_orderid)
        if not record and order.reference.startswith("ACRL:"):
            # Some gateways publish SUBMITTING synchronously inside send_order.
            self.coordinator.process_order(order)
            record = self.coordinator.child_orders.get(order.vt_orderid)
        if not record:
            return
        parent = self.parents.get(record.parent_id)
        if not parent:
            return
        tranche = self._find_tranche(parent, record.tranche_id)

        try:
            self.coordinator.process_order(order)
            market = self._market_for(parent, monotonic())
            if not market:
                return
            if (
                parent.status == ParentStatus.RUNNING
                and tranche.status == TrancheStatus.ACTIVE
            ):
                self.coordinator.reconcile_pending(parent, tranche, market)
            elif (
                parent.status == ParentStatus.RUNNING
                and tranche.status == TrancheStatus.CLOSING
            ):
                self.coordinator.continue_tranche_close(parent, tranche, market)
        except Exception as exc:
            self._pause_on_error(parent, "order event", exc)

    def process_trade_event(self, event: Event) -> None:
        # OrderData.cum_qty is authoritative; consuming TradeData too would double count.
        return

    def process_timer_event(self, event: Event) -> None:
        now = monotonic()
        with self._lock:
            for parent in list(self.parents.values()):
                if parent.status != ParentStatus.RUNNING:
                    continue
                try:
                    self._process_running_parent(parent, now)
                except Exception as exc:
                    self._pause_on_error(parent, "timer event", exc)

    def _process_running_parent(self, parent: ParentOrder, now: float) -> None:
        gateway = self.main_engine.get_gateway(parent.request.gateway_name)
        if gateway is None or getattr(gateway, "is_logged_on", None) is not True:
            self.coordinator.pause_parent(
                parent,
                f"Gateway {parent.request.gateway_name} is not logged on",
            )
            return

        self._update_synthetic_market(parent, now)
        market = self._market_for(parent, now)
        if not market:
            return
        if now - market.timestamp > parent.request.max_market_age_seconds:
            self.coordinator.pause_parent(
                parent,
                f"Market data stale by {now - market.timestamp:.2f}s",
            )
            return
        if not market.has_complete_five_level_book:
            self.coordinator.pause_parent(
                parent,
                "RL model requires a complete ordered five-level book",
            )
            return

        for tranche in self.scheduler.due_to_start(parent, now):
            tranche.activate(now, parent.started_at)
            tranche.message = "Active"
            self.emit_tranche(tranche)

        if now >= parent.deadline_at:
            for tranche in parent.tranches:
                if tranche.status == TrancheStatus.WAITING:
                    tranche.activate(now, parent.started_at)
                self.coordinator.begin_tranche_close(parent, tranche, market)
            return

        for tranche in parent.tranches:
            if tranche.status == TrancheStatus.ACTIVE:
                tranche_market = self._market_for(
                    parent,
                    now,
                    self._tranche_flow_window(tranche),
                )
                if not tranche_market:
                    continue
                if tranche.deadline_at is not None and now >= tranche.deadline_at:
                    self.coordinator.begin_tranche_close(
                        parent,
                        tranche,
                        tranche_market,
                    )
                    continue
                if tranche.decision_due(now):
                    self._run_decision(parent, tranche, tranche_market, now)
                else:
                    self.coordinator.reconcile_pending(
                        parent,
                        tranche,
                        tranche_market,
                    )
            elif tranche.status == TrancheStatus.CLOSING:
                self.coordinator.continue_tranche_close(parent, tranche, market)

    def _pause_on_error(
        self,
        parent: ParentOrder,
        source: str,
        error: Exception,
    ) -> None:
        message = f"{source} failed: {type(error).__name__}: {error}"
        self.write_log(f"Parent {parent.parent_id} {message}")
        self.coordinator.pause_parent(parent, message)

    def _run_decision(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        market: MarketSnapshot,
        now: float,
    ) -> None:
        order_state = self.coordinator.order_state(parent, tranche, market)
        observation = self.observation_builder.build(
            tranche,
            market,
            order_state,
            now,
            market_tick_size=parent.request.price_tick,
        )
        policy = self.parent_policies[parent.parent_id]
        intent = policy.predict(observation)
        target = self.coordinator.apply_intent(
            parent,
            tranche,
            intent,
            market,
        )
        record = IntentRecord(
            parent_id=parent.parent_id,
            tranche_id=tranche.tranche_id,
            normalized_time=tranche.normalized_time(now),
            remaining=tranche.remaining,
            intent=intent,
            target=target,
        )
        tranche.record_decision()
        tranche.message = (
            f"Decision {tranche.decision_index}/10 "
            f"market={target.market_sell} limits={target.limit_sell_levels} "
            f"inactive={target.inactive}"
        )
        self.event_engine.put(Event(EVENT_ACRL_INTENT, record))
        self.emit_tranche(tranche)

    def _update_synthetic_market(self, parent: ParentOrder, now: float) -> None:
        if not parent.request.use_synthetic_market:
            return
        tick = self.synthetic_feed.next_tick(
            parent.request.symbol,
            parent.request.exchange,
            parent.request.price_tick,
        )
        self.market_cache.update_tick(tick, timestamp=now)

    def _market_for(
        self,
        parent: ParentOrder,
        now: float,
        window_seconds: float | None = None,
    ) -> MarketSnapshot | None:
        vt_symbol = f"{parent.request.symbol}.{parent.request.exchange.value}"
        window = (
            max(parent.request.duration_seconds / 10.0, 0.1)
            if window_seconds is None
            else max(window_seconds, 0.1)
        )
        market = self.market_cache.snapshot(vt_symbol, window, now)
        if not market and vt_symbol not in self._missing_market_logged:
            self._missing_market_logged.add(vt_symbol)
            self.write_log(f"Waiting for market data: {vt_symbol}")
        return market

    @staticmethod
    def _tranche_flow_window(tranche: Tranche) -> float:
        """Map the model's 15/150 training window onto one tranche horizon."""

        if tranche.started_at is None or tranche.deadline_at is None:
            raise RuntimeError("cannot compute flow window for an inactive tranche")
        return max((tranche.deadline_at - tranche.started_at) / 10.0, 0.1)

    def _load_policy(
        self,
        rlte_root: str | Path,
        model_path: str | Path,
    ) -> RltePolicyAdapter:
        root = Path(rlte_root).expanduser().resolve()
        model = Path(model_path).expanduser().resolve()
        key = (root, model)
        policy = self._policy_cache.get(key)
        if policy is None:
            expected_sha256 = (
                DEFAULT_MODEL_SHA256
                if model == DEFAULT_MODEL_PATH.resolve()
                else None
            )
            policy = RltePolicyAdapter.load(
                root,
                model,
                expected_sha256=expected_sha256,
            )
            self._policy_cache[key] = policy
            self.write_log(f"RLTE policy loaded: {model}")
        return policy

    def emit_parent(self, parent: ParentOrder) -> None:
        self.event_engine.put(Event(EVENT_ACRL_PARENT, parent))

    def emit_tranche(self, tranche: Tranche) -> None:
        self.event_engine.put(Event(EVENT_ACRL_TRANCHE, tranche))

    def write_log(self, message: str) -> None:
        self.main_engine.write_log(message, APP_NAME)

    def close(self) -> None:
        for parent in list(self.parents.values()):
            if parent.status in {ParentStatus.RUNNING, ParentStatus.PAUSED}:
                self.coordinator.cancel_parent(parent)
        self.event_engine.unregister(EVENT_TIMER, self.process_timer_event)
        self.event_engine.unregister(EVENT_TICK, self.process_tick_event)
        self.event_engine.unregister(EVENT_ORDER, self.process_order_event)
        self.event_engine.unregister(EVENT_TRADE, self.process_trade_event)

    def _get_parent(self, parent_id: str) -> ParentOrder:
        parent = self.parents.get(parent_id)
        if not parent:
            raise KeyError(f"unknown parent order: {parent_id}")
        return parent

    @staticmethod
    def _find_tranche(parent: ParentOrder, tranche_id: str) -> Tranche:
        for tranche in parent.tranches:
            if tranche.tranche_id == tranche_id:
                return tranche
        raise KeyError(f"unknown tranche: {tranche_id}")
