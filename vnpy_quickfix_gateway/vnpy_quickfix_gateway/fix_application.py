from __future__ import annotations

from datetime import UTC, datetime
from typing import Callable

import quickfix as fix
import quickfix42 as fix42

from vnpy_quickfix_gateway.mapping import (
    FIX_SIDE_NAMES,
    FixExecutionReport,
    FixMarketDataSnapshot,
    describe_fix_value,
    execution_report_from_fix,
    fix_side_from_text,
    market_data_snapshot_from_fix,
)


SOH = "\x01"


def format_fix_message(message: fix.Message) -> str:
    """Return a terminal-friendly FIX message string."""
    text = message.toString() if hasattr(message, "toString") else str(message)
    return text.replace(SOH, "|")


def split_message_session(arg0, arg1) -> tuple[fix.Message, fix.SessionID]:
    """Support both common QuickFIX Python callback argument orders."""
    if hasattr(arg0, "getHeader"):
        return arg0, arg1
    return arg1, arg0


def get_msg_type(message: fix.Message) -> str:
    msg_type = fix.MsgType()
    message.getHeader().getField(msg_type)
    return msg_type.getValue()


class FixApplication(fix.Application):
    """Minimal QuickFIX application used to verify Logon/Logout first."""

    def __init__(
        self,
        on_execution_report: Callable[[FixExecutionReport], None] | None = None,
        on_market_data_snapshot: Callable[[FixMarketDataSnapshot], None] | None = None,
    ) -> None:
        super().__init__()
        self.session_id: fix.SessionID | None = None
        self.logged_on = False
        self.execution_reports: list[FixExecutionReport] = []
        self.market_data_snapshots: list[FixMarketDataSnapshot] = []
        self._on_execution_report = on_execution_report
        self._on_market_data_snapshot = on_market_data_snapshot

    def onCreate(self, session_id: fix.SessionID) -> None:
        print(f"[FIX] onCreate: {session_id}", flush=True)

    def onLogon(self, session_id: fix.SessionID) -> None:
        self.session_id = session_id
        self.logged_on = True
        print(f"[FIX] onLogon: {session_id}", flush=True)

    def onLogout(self, session_id: fix.SessionID) -> None:
        self.logged_on = False
        print(f"[FIX] onLogout: {session_id}", flush=True)

    def toAdmin(self, arg0, arg1) -> None:
        message, session_id = split_message_session(arg0, arg1)
        print(f"[FIX] toAdmin {session_id}: {format_fix_message(message)}", flush=True)

    def fromAdmin(self, arg0, arg1) -> None:
        message, session_id = split_message_session(arg0, arg1)
        print(f"[FIX] fromAdmin {session_id}: {format_fix_message(message)}", flush=True)

    def toApp(self, arg0, arg1) -> None:
        message, session_id = split_message_session(arg0, arg1)
        print(f"[FIX] toApp {session_id}: {format_fix_message(message)}", flush=True)

    def fromApp(self, arg0, arg1) -> None:
        message, session_id = split_message_session(arg0, arg1)
        msg_type = get_msg_type(message)
        print(
            f"[FIX] fromApp {session_id} MsgType={msg_type}: "
            f"{format_fix_message(message)}",
            flush=True,
        )
        if msg_type == fix.MsgType_ExecutionReport:
            self.on_execution_report(message)
        elif msg_type == fix.MsgType_MarketDataSnapshotFullRefresh:
            self.on_market_data_snapshot(message)

    def on_execution_report(self, message: fix.Message) -> None:
        report = execution_report_from_fix(message)
        self.execution_reports.append(report)
        print(f"[ORDER] ExecutionReport {report.summary()}", flush=True)
        if self._on_execution_report:
            self._on_execution_report(report)

    def on_market_data_snapshot(self, message: fix.Message) -> None:
        snapshot = market_data_snapshot_from_fix(message)
        self.market_data_snapshots.append(snapshot)
        print(f"[MD] MarketDataSnapshot {snapshot.summary()}", flush=True)
        if self._on_market_data_snapshot:
            self._on_market_data_snapshot(snapshot)

    def send_new_order_single(
        self,
        symbol: str,
        side: str,
        price: float,
        quantity: float,
        cl_ord_id: str | None = None,
    ) -> str:
        if not self.session_id or not self.logged_on:
            raise RuntimeError("FIX session is not logged on")

        order_id = cl_ord_id or self.generate_cl_ord_id()
        fix_side = fix_side_from_text(side)
        message = fix42.NewOrderSingle()
        message.setField(fix.ClOrdID(order_id))
        message.setField(fix.HandlInst(fix.HandlInst_AUTOMATED_EXECUTION_NO_INTERVENTION))
        message.setField(fix.Symbol(symbol))
        message.setField(fix.Side(fix_side))
        message.setField(fix.TransactTime())
        message.setField(fix.OrderQty(quantity))
        message.setField(fix.OrdType(fix.OrdType_LIMIT))
        message.setField(fix.Price(price))
        message.setField(fix.TimeInForce(fix.TimeInForce_DAY))

        fix.Session.sendToTarget(message, self.session_id)
        side_text = describe_fix_value(fix_side, FIX_SIDE_NAMES)
        print(
            f"[ORDER] sent NewOrderSingle ClOrdID={order_id} "
            f"{symbol} {side_text} {quantity}@{price}",
            flush=True,
        )
        return order_id

    def send_order_cancel_request(
        self,
        orig_cl_ord_id: str,
        symbol: str,
        side: str,
        cl_ord_id: str | None = None,
    ) -> str:
        if not self.session_id or not self.logged_on:
            raise RuntimeError("FIX session is not logged on")

        cancel_id = cl_ord_id or f"{self.generate_cl_ord_id()}C"
        fix_side = fix_side_from_text(side)
        message = fix42.OrderCancelRequest()
        message.setField(fix.OrigClOrdID(orig_cl_ord_id))
        message.setField(fix.ClOrdID(cancel_id))
        message.setField(fix.Symbol(symbol))
        message.setField(fix.Side(fix_side))
        message.setField(fix.TransactTime())

        fix.Session.sendToTarget(message, self.session_id)
        side_text = describe_fix_value(fix_side, FIX_SIDE_NAMES)
        print(
            f"[ORDER] sent OrderCancelRequest ClOrdID={cancel_id} "
            f"OrigClOrdID={orig_cl_ord_id} {symbol} {side_text}",
            flush=True,
        )
        return cancel_id

    def send_market_data_request(
        self,
        symbol: str,
        md_req_id: str | None = None,
    ) -> str:
        if not self.session_id or not self.logged_on:
            raise RuntimeError("FIX session is not logged on")

        request_id = md_req_id or f"MD{self.generate_cl_ord_id()}"
        message = fix42.MarketDataRequest()
        message.setField(fix.MDReqID(request_id))
        message.setField(
            fix.SubscriptionRequestType(fix.SubscriptionRequestType_SNAPSHOT)
        )
        message.setField(fix.MarketDepth(5))

        for entry_type in (
            fix.MDEntryType_BID,
            fix.MDEntryType_OFFER,
            fix.MDEntryType_TRADE,
        ):
            group = fix42.MarketDataRequest.NoMDEntryTypes()
            group.setField(fix.MDEntryType(entry_type))
            message.addGroup(group)

        symbol_group = fix42.MarketDataRequest.NoRelatedSym()
        symbol_group.setField(fix.Symbol(symbol))
        message.addGroup(symbol_group)

        fix.Session.sendToTarget(message, self.session_id)
        print(
            f"[MD] sent MarketDataRequest MDReqID={request_id} Symbol={symbol}",
            flush=True,
        )
        return request_id

    @staticmethod
    def generate_cl_ord_id() -> str:
        return datetime.now(UTC).strftime("VN%Y%m%d%H%M%S%f")
