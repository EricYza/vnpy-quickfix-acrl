from __future__ import annotations

from copy import copy
from dataclasses import dataclass
from pathlib import Path

import quickfix as fix

from vnpy.event import EventEngine
from vnpy.trader.constant import Exchange, Offset, OrderType, Status
from vnpy.trader.gateway import BaseGateway
from vnpy.trader.object import (
    CancelRequest,
    OrderData,
    OrderRequest,
    SubscribeRequest,
)

from vnpy_quickfix_gateway.fix_application import FixApplication
from vnpy_quickfix_gateway.mapping import (
    FixExecutionReport,
    FixMarketDataSnapshot,
    fix_side_from_direction,
    order_data_from_execution_report,
    tick_data_from_market_data_snapshot,
    trade_data_from_execution_report,
)


DEFAULT_CONFIG = Path(__file__).with_name("quickfix_client.cfg")


@dataclass(frozen=True)
class OrderContext:
    exchange: Exchange
    order_type: OrderType
    offset: Offset
    price: float
    reference: str


class QuickfixGateway(BaseGateway):
    """Minimal vn.py gateway backed by QuickFIX SocketInitiator."""

    default_name = "QUICKFIX"
    default_setting = {
        "配置文件": str(DEFAULT_CONFIG),
    }
    exchanges = [Exchange.LOCAL]

    def __init__(self, event_engine: EventEngine, gateway_name: str) -> None:
        super().__init__(event_engine, gateway_name)
        self.application: FixApplication | None = None
        self.initiator: fix.SocketInitiator | None = None
        self.orders: dict[str, OrderData] = {}
        self.order_contexts: dict[str, OrderContext] = {}
        self.subscription_exchanges: dict[str, Exchange] = {}

    @property
    def is_logged_on(self) -> bool:
        """Return whether the QuickFIX initiator has an active FIX session."""

        return bool(self.application and self.application.logged_on)

    def connect(self, setting: dict) -> None:
        config_path = self.get_config_path(setting)
        settings = fix.SessionSettings(str(config_path))

        self.application = FixApplication(
            on_execution_report=self.on_fix_execution_report,
            on_market_data_snapshot=self.on_fix_market_data_snapshot,
        )
        store_factory = fix.FileStoreFactory(settings)
        log_factory = fix.FileLogFactory(settings)
        self.initiator = fix.SocketInitiator(
            self.application,
            store_factory,
            settings,
            log_factory,
        )
        self.initiator.start()
        self.write_log(f"QuickFIX initiator started: {config_path}")

    def close(self) -> None:
        if self.initiator:
            self.initiator.stop()
            self.initiator = None
            self.write_log("QuickFIX initiator stopped")

        self.application = None

    def subscribe(self, req: SubscribeRequest) -> None:
        self.subscription_exchanges[req.symbol] = req.exchange

        if not self.application or not self.application.logged_on:
            self.write_log("Cannot subscribe before FIX Logon")
            return

        try:
            self.application.send_market_data_request(req.symbol)
        except Exception as exc:
            self.write_log(f"Failed to subscribe market data: {exc}")

    def send_order(self, req: OrderRequest) -> str:
        orderid = FixApplication.generate_cl_ord_id()
        order = req.create_order_data(orderid, self.gateway_name)
        order.status = Status.SUBMITTING
        self.orders[orderid] = order
        self.order_contexts[orderid] = OrderContext(
            exchange=req.exchange,
            order_type=req.type,
            offset=req.offset,
            price=req.price,
            reference=req.reference,
        )
        self.on_order(copy(order))

        if req.type != OrderType.LIMIT:
            order.status = Status.REJECTED
            self.on_order(copy(order))
            self.write_log(f"Only LIMIT orders are supported now: {req.type}")
            return order.vt_orderid

        if not self.application or not self.application.logged_on:
            order.status = Status.REJECTED
            self.on_order(copy(order))
            self.write_log("Cannot send order before FIX Logon")
            return order.vt_orderid

        try:
            self.application.send_new_order_single(
                symbol=req.symbol,
                side=fix_side_from_direction(req.direction),
                price=req.price,
                quantity=req.volume,
                cl_ord_id=orderid,
            )
        except Exception as exc:
            order.status = Status.REJECTED
            self.on_order(copy(order))
            self.write_log(f"Failed to send order: {exc}")

        return order.vt_orderid

    def cancel_order(self, req: CancelRequest) -> None:
        order = self.orders.get(req.orderid)
        if not order:
            self.write_log(f"Cannot cancel unknown order: {req.orderid}")
            return

        if not order.direction:
            self.write_log(f"Cannot cancel order without direction: {req.orderid}")
            return

        if not self.application or not self.application.logged_on:
            self.write_log("Cannot cancel order before FIX Logon")
            return

        try:
            self.application.send_order_cancel_request(
                orig_cl_ord_id=req.orderid,
                symbol=req.symbol,
                side=fix_side_from_direction(order.direction),
            )
        except Exception as exc:
            self.write_log(f"Failed to cancel order: {exc}")

    def query_account(self) -> None:
        self.write_log("Account query is not implemented yet")

    def query_position(self) -> None:
        self.write_log("Position query is not implemented yet")

    def on_fix_execution_report(self, report: FixExecutionReport) -> None:
        context = self.order_contexts.get(report.orderid)
        exchange = context.exchange if context else Exchange.LOCAL
        order_type = context.order_type if context else OrderType.LIMIT
        offset = context.offset if context else Offset.NONE
        price = context.price if context else 0
        reference = context.reference if context else ""

        order = order_data_from_execution_report(
            report,
            gateway_name=self.gateway_name,
            exchange=exchange,
            order_type=order_type,
            offset=offset,
            price=price,
            reference=reference,
        )
        self.orders[order.orderid] = order
        self.on_order(order)

        trade = trade_data_from_execution_report(
            report,
            gateway_name=self.gateway_name,
            exchange=exchange,
            offset=offset,
        )
        if trade:
            self.on_trade(trade)

    def on_fix_market_data_snapshot(self, snapshot: FixMarketDataSnapshot) -> None:
        exchange = self.subscription_exchanges.get(snapshot.symbol, Exchange.LOCAL)
        tick = tick_data_from_market_data_snapshot(
            snapshot,
            gateway_name=self.gateway_name,
            exchange=exchange,
        )
        self.on_tick(tick)

    @staticmethod
    def get_config_path(setting: dict) -> Path:
        value = (
            setting.get("配置文件")
            or setting.get("config_path")
            or setting.get("config")
            or DEFAULT_CONFIG
        )
        return Path(value).expanduser().resolve()
