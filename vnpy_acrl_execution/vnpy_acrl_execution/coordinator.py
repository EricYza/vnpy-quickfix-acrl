from __future__ import annotations

from dataclasses import dataclass
from threading import RLock
from typing import Callable

from vnpy.trader.constant import Direction, Offset, OrderType, Status
from vnpy.trader.engine import MainEngine
from vnpy.trader.object import CancelRequest, OrderData, OrderRequest

from .market_state import MarketSnapshot
from .models import (
    ExecutionIntent,
    ParentOrder,
    ParentStatus,
    TargetAllocation,
    Tranche,
    TrancheOrderState,
    TrancheStatus,
)


ACTIVE_STATUSES = {Status.SUBMITTING, Status.NOTTRADED, Status.PARTTRADED}
FINAL_STATUSES = {Status.ALLTRADED, Status.CANCELLED, Status.REJECTED}


@dataclass
class ChildOrderRecord:
    vt_orderid: str
    orderid: str
    parent_id: str
    tranche_id: str
    symbol: str
    exchange: object
    gateway_name: str
    price: float
    volume: int
    requested_level: int
    aggressive: bool
    reference: str
    traded: int = 0
    status: Status = Status.SUBMITTING
    cancel_requested: bool = False

    @property
    def leaves(self) -> int:
        if self.status not in ACTIVE_STATUSES:
            return 0
        return max(self.volume - self.traded, 0)


@dataclass
class ReconciliationTarget:
    allocation: TargetAllocation
    market: MarketSnapshot


class ExecutionCoordinator:
    """The only component allowed to turn RL intents into child orders."""

    def __init__(
        self,
        main_engine: MainEngine,
        on_parent_changed: Callable[[ParentOrder], None],
        on_tranche_changed: Callable[[Tranche], None],
        write_log: Callable[[str], None],
    ) -> None:
        self.main_engine = main_engine
        self.on_parent_changed = on_parent_changed
        self.on_tranche_changed = on_tranche_changed
        self.write_log = write_log
        self.parents: dict[str, ParentOrder] = {}
        self.child_orders: dict[str, ChildOrderRecord] = {}
        self.targets: dict[str, ReconciliationTarget] = {}
        self._lock = RLock()

    def register_parent(self, parent: ParentOrder) -> None:
        with self._lock:
            self.parents[parent.parent_id] = parent

    def apply_intent(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        intent: ExecutionIntent,
        market: MarketSnapshot,
    ) -> TargetAllocation:
        with self._lock:
            target = intent.target_quantities(tranche.remaining)
            self.targets[tranche.tranche_id] = ReconciliationTarget(
                allocation=target,
                market=market,
            )
            self._reconcile(parent, tranche)
            return target

    def reconcile_pending(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        market: MarketSnapshot,
    ) -> None:
        with self._lock:
            target = self.targets.get(tranche.tranche_id)
            if not target or tranche.status != TrancheStatus.ACTIVE:
                return
            target.market = market
            self._reconcile(parent, tranche)

    def order_state(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        market: MarketSnapshot,
    ) -> TrancheOrderState:
        with self._lock:
            by_level = [0, 0, 0, 0, 0]
            outside = 0
            aggressive = 0
            queue_ahead = [0.0, 0.0, 0.0, 0.0, 0.0]

            for order in self._orders_for_tranche(tranche.tranche_id):
                if order.leaves <= 0:
                    continue
                if order.aggressive:
                    aggressive += order.leaves
                    continue

                level = self._observation_level(
                    parent,
                    order.price,
                    market.best_ask,
                )
                if 1 <= level <= 5:
                    by_level[level - 1] += order.leaves
                else:
                    outside += order.leaves

            for level in range(5):
                # Assuming our passive orders are at the back of a
                # market-by-price level, external quantity is the closest
                # available estimate of queue ahead.  True FIFO reconstruction
                # requires market-by-order data.
                level_price = (
                    market.best_ask + level * parent.request.price_tick
                )
                total_depth = market.depth_at_price(
                    "ask",
                    level_price,
                    parent.request.price_tick,
                )
                queue_ahead[level] = max(
                    total_depth - by_level[level],
                    0.0,
                )

            return TrancheOrderState(
                active_by_level=tuple(by_level),
                active_outside=outside,
                pending_aggressive=aggressive,
                queue_ahead_by_level=tuple(queue_ahead),
            )

    def begin_tranche_close(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        market: MarketSnapshot,
    ) -> None:
        with self._lock:
            if tranche.status in {
                TrancheStatus.COMPLETED,
                TrancheStatus.CANCELLED,
                TrancheStatus.ERROR,
            }:
                return
            if tranche.status == TrancheStatus.CLOSING:
                self._finish_closing_tranche(parent, tranche, market)
                return
            tranche.status = TrancheStatus.CLOSING
            tranche.message = "Deadline reached; cancelling active child orders"
            self.targets.pop(tranche.tranche_id, None)
            self._cancel_tranche_orders(tranche)
            self._finish_closing_tranche(parent, tranche, market)
            self.on_tranche_changed(tranche)

    def continue_tranche_close(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        market: MarketSnapshot,
    ) -> None:
        with self._lock:
            if tranche.status == TrancheStatus.CLOSING:
                self._finish_closing_tranche(parent, tranche, market)

    def pause_parent(self, parent: ParentOrder, message: str) -> None:
        with self._lock:
            if parent.status not in {ParentStatus.RUNNING, ParentStatus.WAITING}:
                return
            parent.status = ParentStatus.PAUSED
            parent.message = message
            for tranche in parent.tranches:
                if tranche.status == TrancheStatus.ACTIVE:
                    tranche.status = TrancheStatus.CLOSING
                self.targets.pop(tranche.tranche_id, None)
                self._cancel_tranche_orders(tranche)
                self.on_tranche_changed(tranche)
            self.write_log(f"Parent {parent.parent_id} paused: {message}")
            self.on_parent_changed(parent)

    def resume_parent(self, parent: ParentOrder) -> None:
        with self._lock:
            if parent.status != ParentStatus.PAUSED:
                raise RuntimeError("only a paused parent can be resumed")
            if any(order.leaves for order in self._orders_for_parent(parent.parent_id)):
                raise RuntimeError("wait for all child-order cancellations before resume")

            parent.status = ParentStatus.RUNNING
            parent.message = "Resumed by user"
            for tranche in parent.tranches:
                if tranche.status == TrancheStatus.CLOSING and tranche.remaining:
                    tranche.status = TrancheStatus.WAITING
                    tranche.started_at = None
                    tranche.deadline_at = None
                    tranche.next_decision_at = None
                    tranche.decision_index = 0
                    tranche.reference_bid = None
                    tranche.reference_mid = None
                    self.on_tranche_changed(tranche)
            self.on_parent_changed(parent)

    def cancel_parent(self, parent: ParentOrder) -> None:
        with self._lock:
            if parent.status in {ParentStatus.COMPLETED, ParentStatus.CANCELLED}:
                return
            parent.status = ParentStatus.CANCELLED
            parent.message = "Cancelled by user"
            for tranche in parent.tranches:
                self.targets.pop(tranche.tranche_id, None)
                self._cancel_tranche_orders(tranche)
                if tranche.status not in {TrancheStatus.COMPLETED, TrancheStatus.ERROR}:
                    tranche.status = TrancheStatus.CANCELLED
                self.on_tranche_changed(tranche)
            self.on_parent_changed(parent)

    def process_order(self, order: OrderData) -> None:
        with self._lock:
            record = self.child_orders.get(order.vt_orderid)
            if not record and order.reference.startswith("ACRL:"):
                record = self._adopt_algorithm_order(order)
            if not record:
                return

            parent = self.parents.get(record.parent_id)
            if not parent:
                return
            tranche = self._find_tranche(parent, record.tranche_id)

            new_traded = int(round(order.traded))
            delta = max(new_traded - record.traded, 0)
            record.traded = max(record.traded, new_traded)
            incoming_status = order.status
            status_accepted = not (
                record.status in FINAL_STATUSES
                and incoming_status != record.status
            )
            if status_accepted:
                record.status = incoming_status

            if delta:
                tranche.traded = min(tranche.traded + delta, tranche.volume)
                parent.traded = min(parent.traded + delta, parent.request.total_volume)
                self._consume_target_fill(parent, record, delta)
                self.write_log(
                    f"Fill parent={parent.parent_id} tranche={tranche.tranche_id} "
                    f"delta={delta} parent_traded={parent.traded}"
                )

            if (
                status_accepted
                and incoming_status == Status.CANCELLED
                and not record.cancel_requested
            ):
                self.pause_parent(
                    parent,
                    f"Algorithm child {order.vt_orderid} was cancelled externally",
                )
                return

            if status_accepted and incoming_status == Status.REJECTED:
                self.pause_parent(
                    parent,
                    f"Algorithm child {order.vt_orderid} was rejected",
                )
                return

            if tranche.remaining == 0:
                tranche.status = TrancheStatus.COMPLETED
                tranche.message = "Filled"
                self.targets.pop(tranche.tranche_id, None)
                self._cancel_tranche_orders(tranche)

            if parent.remaining == 0:
                parent.status = ParentStatus.COMPLETED
                parent.message = "Parent order completed"

            self.on_tranche_changed(tranche)
            self.on_parent_changed(parent)

    def assert_hard_limits(self, parent: ParentOrder) -> None:
        potential = sum(order.leaves for order in self._orders_for_parent(parent.parent_id))
        if parent.traded + potential > parent.request.total_volume:
            raise RuntimeError(
                f"parent hard limit violated: traded={parent.traded}, potential={potential}, "
                f"total={parent.request.total_volume}"
            )

        for tranche in parent.tranches:
            tranche_potential = sum(
                order.leaves for order in self._orders_for_tranche(tranche.tranche_id)
            )
            if tranche.traded + tranche_potential > tranche.volume:
                raise RuntimeError(
                    f"tranche hard limit violated for {tranche.tranche_id}: "
                    f"traded={tranche.traded}, potential={tranche_potential}, "
                    f"total={tranche.volume}"
                )

    def _reconcile(self, parent: ParentOrder, tranche: Tranche) -> None:
        target = self.targets.get(tranche.tranche_id)
        if not target or parent.status != ParentStatus.RUNNING:
            return

        market = target.market
        active_by_level: dict[int, list[ChildOrderRecord]] = {
            level: [] for level in range(1, 6)
        }
        active_aggressive: list[ChildOrderRecord] = []
        outside: list[ChildOrderRecord] = []
        for order in self._orders_for_tranche(tranche.tranche_id):
            if order.leaves <= 0:
                continue
            if order.aggressive:
                if self._current_level(parent, order.price, market.best_bid) == 0:
                    active_aggressive.append(order)
                else:
                    outside.append(order)
                continue
            level = self._current_level(parent, order.price, market.best_bid)
            if 1 <= level <= 5:
                active_by_level[level].append(order)
            else:
                outside.append(order)

        for order in outside:
            self._cancel_order(order)

        current_aggressive = sum(order.leaves for order in active_aggressive)
        if current_aggressive > target.allocation.market_sell:
            for order in reversed(active_aggressive):
                if current_aggressive <= target.allocation.market_sell:
                    break
                self._cancel_order(order)
                current_aggressive -= order.leaves

        for level, desired in enumerate(target.allocation.limit_sell_levels, start=1):
            orders = active_by_level[level]
            current = sum(order.leaves for order in orders)
            if current > desired:
                for order in reversed(orders):
                    if current <= desired:
                        break
                    self._cancel_order(order)
                    current -= order.leaves

        cancel_pending = any(
            order.cancel_requested and order.leaves > 0
            for order in self._orders_for_tranche(tranche.tranche_id)
        )
        if cancel_pending:
            self.assert_hard_limits(parent)
            return

        current_aggressive = sum(
            order.leaves
            for order in self._orders_for_tranche(tranche.tranche_id)
            if order.aggressive
            and self._current_level(parent, order.price, market.best_bid) == 0
        )
        aggressive_quantity = min(
            target.allocation.market_sell - current_aggressive,
            self._available(parent, tranche),
        )
        if aggressive_quantity > 0:
            self._send_order(
                parent,
                tranche,
                aggressive_quantity,
                market.best_bid,
                0,
                True,
            )

        for level, desired in enumerate(target.allocation.limit_sell_levels, start=1):
            current = sum(
                order.leaves
                for order in self._orders_for_tranche(tranche.tranche_id)
                if not order.aggressive
                and self._current_level(parent, order.price, market.best_bid) == level
            )
            quantity = min(desired - current, self._available(parent, tranche))
            if quantity > 0:
                price = market.best_bid + level * parent.request.price_tick
                self._send_order(parent, tranche, quantity, price, level, False)

        self.assert_hard_limits(parent)

    def _consume_target_fill(
        self,
        parent: ParentOrder,
        order: ChildOrderRecord,
        quantity: int,
    ) -> None:
        """Remove a fill from the current target so it is never replenished.

        A policy action is a target allocation for inventory remaining at one
        decision instant.  Fills consume that allocation.  They must not be
        interpreted as a new deficit before the next policy decision.
        """

        target = self.targets.get(order.tranche_id)
        if not target or quantity <= 0:
            return

        allocation = target.allocation
        quantities = [
            allocation.market_sell,
            *allocation.limit_sell_levels,
            allocation.inactive,
        ]
        if order.aggressive:
            preferred = 0
        else:
            level = self._current_level(
                parent,
                order.price,
                target.market.best_bid,
            )
            preferred = level if 1 <= level <= 5 else 6

        # A cancel/fill race can execute an order no longer represented by the
        # newest target.  Consume inactive inventory first, then the remaining
        # buckets deterministically, while preserving total-volume accounting.
        priority = [preferred, 6, 0, 1, 2, 3, 4, 5]
        remaining = quantity
        for index in dict.fromkeys(priority):
            consumed = min(quantities[index], remaining)
            quantities[index] -= consumed
            remaining -= consumed
            if remaining == 0:
                break
        if remaining:
            raise RuntimeError(
                f"fill exceeds reconciliation target by {remaining} lots"
            )

        target.allocation = TargetAllocation(
            market_sell=quantities[0],
            limit_sell_levels=tuple(quantities[1:6]),
            inactive=quantities[6],
        )

    def _finish_closing_tranche(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        market: MarketSnapshot,
    ) -> None:
        if tranche.remaining <= 0:
            tranche.status = TrancheStatus.COMPLETED
            tranche.message = "Filled"
            return

        active = [
            order for order in self._orders_for_tranche(tranche.tranche_id) if order.leaves
        ]
        if active:
            for order in active:
                if (
                    not order.aggressive
                    or self._current_level(parent, order.price, market.best_bid) != 0
                ):
                    self._cancel_order(order)
            if any(order.cancel_requested for order in active):
                tranche.message = "Deadline fallback waiting for cancel confirmations"
            else:
                tranche.message = "Deadline fallback working at current best bid"
            return

        quantity = min(tranche.remaining, self._available(parent, tranche))
        if quantity > 0:
            tranche.message = "Deadline fallback sent as marketable limit"
            self._send_order(parent, tranche, quantity, market.best_bid, 0, True)

    def _send_order(
        self,
        parent: ParentOrder,
        tranche: Tranche,
        quantity: int,
        price: float,
        level: int,
        aggressive: bool,
    ) -> ChildOrderRecord | None:
        if quantity <= 0:
            return None
        reference = f"ACRL:{parent.parent_id}:{tranche.tranche_id}:L{level}"
        request = OrderRequest(
            symbol=parent.request.symbol,
            exchange=parent.request.exchange,
            direction=Direction.SHORT,
            type=OrderType.LIMIT,
            volume=quantity,
            price=price,
            offset=Offset.NONE,
            reference=reference,
        )
        vt_orderid = self.main_engine.send_order(request, parent.request.gateway_name)
        if not vt_orderid:
            self.pause_parent(parent, "Gateway returned an empty child order id")
            return None
        orderid = vt_orderid.split(".", 1)[-1]
        record = self.child_orders.get(vt_orderid)
        if record is None:
            record = ChildOrderRecord(
                vt_orderid=vt_orderid,
                orderid=orderid,
                parent_id=parent.parent_id,
                tranche_id=tranche.tranche_id,
                symbol=parent.request.symbol,
                exchange=parent.request.exchange,
                gateway_name=parent.request.gateway_name,
                price=price,
                volume=quantity,
                requested_level=level,
                aggressive=aggressive,
                reference=reference,
            )
            self.child_orders[vt_orderid] = record
        self.write_log(
            f"Child sent parent={parent.parent_id} tranche={tranche.tranche_id} "
            f"qty={quantity} price={price} level={level} aggressive={aggressive}"
        )
        return record

    def _cancel_order(self, order: ChildOrderRecord) -> None:
        if order.leaves <= 0 or order.cancel_requested:
            return
        order.cancel_requested = True
        request = CancelRequest(
            orderid=order.orderid,
            symbol=order.symbol,
            exchange=order.exchange,
        )
        self.main_engine.cancel_order(request, order.gateway_name)

    def _adopt_algorithm_order(self, order: OrderData) -> ChildOrderRecord | None:
        parts = order.reference.split(":")
        if len(parts) != 4 or parts[0] != "ACRL" or not parts[3].startswith("L"):
            self.write_log(
                f"Ignoring malformed algorithm order reference: {order.reference}"
            )
            return None
        try:
            level = int(parts[3][1:])
        except ValueError:
            self.write_log(
                f"Ignoring malformed algorithm order level: {order.reference}"
            )
            return None

        parent_id = parts[1]
        tranche_id = parts[2]
        if parent_id not in self.parents:
            return None
        record = ChildOrderRecord(
            vt_orderid=order.vt_orderid,
            orderid=order.orderid,
            parent_id=parent_id,
            tranche_id=tranche_id,
            symbol=order.symbol,
            exchange=order.exchange,
            gateway_name=order.gateway_name,
            price=order.price,
            volume=int(round(order.volume)),
            requested_level=level,
            aggressive=level == 0,
            reference=order.reference,
            # Start at zero so a first-seen partially filled event contributes
            # its cumulative fill through the normal delta calculation.
            traded=0,
            status=order.status,
        )
        self.child_orders[order.vt_orderid] = record
        return record

    def _cancel_tranche_orders(self, tranche: Tranche) -> None:
        for order in self._orders_for_tranche(tranche.tranche_id):
            self._cancel_order(order)

    def _available(self, parent: ParentOrder, tranche: Tranche) -> int:
        parent_potential = sum(
            order.leaves for order in self._orders_for_parent(parent.parent_id)
        )
        tranche_potential = sum(
            order.leaves for order in self._orders_for_tranche(tranche.tranche_id)
        )
        parent_available = parent.request.total_volume - parent.traded - parent_potential
        tranche_available = tranche.volume - tranche.traded - tranche_potential
        return max(min(parent_available, tranche_available), 0)

    def _orders_for_parent(self, parent_id: str) -> list[ChildOrderRecord]:
        return [
            order for order in self.child_orders.values() if order.parent_id == parent_id
        ]

    def _orders_for_tranche(self, tranche_id: str) -> list[ChildOrderRecord]:
        return [
            order for order in self.child_orders.values() if order.tranche_id == tranche_id
        ]

    @staticmethod
    def _find_tranche(parent: ParentOrder, tranche_id: str) -> Tranche:
        for tranche in parent.tranches:
            if tranche.tranche_id == tranche_id:
                return tranche
        raise KeyError(f"unknown tranche: {tranche_id}")

    @staticmethod
    def _current_level(parent: ParentOrder, price: float, best_bid: float) -> int:
        raw_level = (price - best_bid) / parent.request.price_tick
        rounded = int(round(raw_level))
        if abs(raw_level - rounded) > 1e-6:
            return 99
        return rounded

    @staticmethod
    def _observation_level(
        parent: ParentOrder,
        price: float,
        best_ask: float,
    ) -> int:
        """Return RLTE's historical best-ask-anchored observation level."""

        raw_offset = (price - best_ask) / parent.request.price_tick
        rounded = int(round(raw_offset))
        if abs(raw_offset - rounded) > 1e-6:
            return 99
        return rounded + 1
