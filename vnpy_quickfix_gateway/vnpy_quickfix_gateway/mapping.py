from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime as Datetime

import quickfix as fix
import quickfix42 as fix42

from vnpy.trader.constant import Direction, Exchange, Offset, OrderType, Status
from vnpy.trader.object import OrderData, TickData, TradeData


FIX_SIDE_NAMES = {
    fix.Side_BUY: "BUY",
    fix.Side_SELL: "SELL",
}

FIX_EXEC_TYPE_NAMES = {
    fix.ExecType_NEW: "NEW",
    fix.ExecType_PARTIAL_FILL: "PARTIAL_FILL",
    fix.ExecType_FILL: "FILL",
    fix.ExecType_CANCELED: "CANCELED",
    fix.ExecType_REJECTED: "REJECTED",
    fix.ExecType_TRADE: "TRADE",
}

FIX_ORDER_STATUS_NAMES = {
    fix.OrdStatus_NEW: "NEW",
    fix.OrdStatus_PARTIALLY_FILLED: "PARTIALLY_FILLED",
    fix.OrdStatus_FILLED: "FILLED",
    fix.OrdStatus_CANCELED: "CANCELED",
    fix.OrdStatus_REJECTED: "REJECTED",
    fix.OrdStatus_PENDING_NEW: "PENDING_NEW",
    fix.OrdStatus_PENDING_CANCEL: "PENDING_CANCEL",
    fix.OrdStatus_PENDING_REPLACE: "PENDING_REPLACE",
    fix.OrdStatus_EXPIRED: "EXPIRED",
}

VNPY_DIRECTION_TO_FIX_SIDE = {
    Direction.LONG: fix.Side_BUY,
    Direction.SHORT: fix.Side_SELL,
}

FIX_SIDE_TO_VNPY_DIRECTION = {
    fix.Side_BUY: Direction.LONG,
    fix.Side_SELL: Direction.SHORT,
}

VNPY_ORDER_TYPE_TO_FIX_ORD_TYPE = {
    OrderType.LIMIT: fix.OrdType_LIMIT,
    OrderType.MARKET: fix.OrdType_MARKET,
    OrderType.STOP: fix.OrdType_STOP,
}

FIX_ORD_TYPE_TO_VNPY_ORDER_TYPE = {
    fix.OrdType_LIMIT: OrderType.LIMIT,
    fix.OrdType_MARKET: OrderType.MARKET,
    fix.OrdType_STOP: OrderType.STOP,
}

FIX_ORD_STATUS_TO_VNPY_STATUS = {
    fix.OrdStatus_PENDING_NEW: Status.SUBMITTING,
    fix.OrdStatus_NEW: Status.NOTTRADED,
    fix.OrdStatus_PARTIALLY_FILLED: Status.PARTTRADED,
    fix.OrdStatus_FILLED: Status.ALLTRADED,
    fix.OrdStatus_CANCELED: Status.CANCELLED,
    fix.OrdStatus_EXPIRED: Status.CANCELLED,
    fix.OrdStatus_REJECTED: Status.REJECTED,
}

TRADE_EXEC_TYPES = {
    fix.ExecType_PARTIAL_FILL,
    fix.ExecType_FILL,
    fix.ExecType_TRADE,
}

MARKET_DATA_ENTRY_NAMES = {
    fix.MDEntryType_BID: "BID",
    fix.MDEntryType_OFFER: "OFFER",
    fix.MDEntryType_TRADE: "TRADE",
}


@dataclass(frozen=True)
class FixExecutionReport:
    """Normalized subset of a FIX ExecutionReport used by the gateway."""

    order_id: str | None
    cl_ord_id: str
    exec_id: str | None
    exec_type: str | None
    ord_status: str | None
    symbol: str
    side: str | None
    order_qty: float
    leaves_qty: float
    cum_qty: float
    avg_px: float
    last_qty: float
    last_px: float
    text: str | None = None

    @property
    def orderid(self) -> str:
        """Use ClOrdID as vn.py local orderid for our generated orders."""
        return self.cl_ord_id

    @property
    def direction(self) -> Direction | None:
        if self.side is None:
            return None
        return direction_from_fix_side(self.side)

    @property
    def status(self) -> Status:
        return status_from_fix_ord_status(self.ord_status)

    @property
    def is_trade(self) -> bool:
        return bool(self.exec_type in TRADE_EXEC_TYPES and self.last_qty > 0)

    def summary(self) -> str:
        fields = {
            "OrderID": self.order_id,
            "ClOrdID": self.cl_ord_id,
            "ExecID": self.exec_id,
            "ExecType": describe_fix_value(self.exec_type, FIX_EXEC_TYPE_NAMES),
            "OrdStatus": describe_fix_value(self.ord_status, FIX_ORDER_STATUS_NAMES),
            "Symbol": self.symbol,
            "Side": describe_fix_value(self.side, FIX_SIDE_NAMES),
            "LeavesQty": self.leaves_qty,
            "CumQty": self.cum_qty,
            "AvgPx": self.avg_px,
            "LastQty": self.last_qty,
            "LastPx": self.last_px,
            "Text": self.text,
        }
        return ", ".join(
            f"{name}={value}" for name, value in fields.items() if value is not None
        )


@dataclass(frozen=True)
class FixMarketDataSnapshot:
    """Normalized five-level subset of MarketDataSnapshotFullRefresh."""

    symbol: str
    md_req_id: str | None = None
    bid_price_1: float = 0
    bid_volume_1: float = 0
    ask_price_1: float = 0
    ask_volume_1: float = 0
    bid_price_2: float = 0
    bid_volume_2: float = 0
    ask_price_2: float = 0
    ask_volume_2: float = 0
    bid_price_3: float = 0
    bid_volume_3: float = 0
    ask_price_3: float = 0
    ask_volume_3: float = 0
    bid_price_4: float = 0
    bid_volume_4: float = 0
    ask_price_4: float = 0
    ask_volume_4: float = 0
    bid_price_5: float = 0
    bid_volume_5: float = 0
    ask_price_5: float = 0
    ask_volume_5: float = 0
    last_price: float = 0
    last_volume: float = 0

    def summary(self) -> str:
        return (
            f"MDReqID={self.md_req_id}, Symbol={self.symbol}, "
            f"Bid={self.bid_price_1}@{self.bid_volume_1}, "
            f"Ask={self.ask_price_1}@{self.ask_volume_1}, "
            f"Last={self.last_price}@{self.last_volume}"
        )


def describe_fix_value(value: str | int | float | None, names: dict[str, str]) -> str | None:
    if value is None:
        return None

    text = str(value)
    name = names.get(text)
    if name:
        return f"{text}({name})"
    return text


def get_field_value(message: fix.Message, field, default=None):
    try:
        message.getField(field)
    except fix.FieldNotFound:
        return default
    return field.getValue()


def get_float_field(message: fix.Message, field, default: float = 0) -> float:
    value = get_field_value(message, field)
    if value is None:
        return default
    return float(value)


def get_last_qty(message: fix.Message) -> float:
    last_shares = get_field_value(message, fix.LastShares())
    if last_shares is not None:
        return float(last_shares)

    last_qty = get_field_value(message, fix.LastQty())
    if last_qty is not None:
        return float(last_qty)

    return 0


def direction_from_fix_side(side: str) -> Direction:
    try:
        return FIX_SIDE_TO_VNPY_DIRECTION[side]
    except KeyError as exc:
        raise ValueError(f"Unsupported FIX Side: {side}") from exc


def fix_side_from_direction(direction: Direction) -> str:
    try:
        return VNPY_DIRECTION_TO_FIX_SIDE[direction]
    except KeyError as exc:
        raise ValueError(f"Unsupported vn.py direction: {direction}") from exc


def fix_side_from_text(side: str) -> str:
    value = side.strip().upper()
    if value in {"1", "BUY", "B"}:
        return fix.Side_BUY
    if value in {"2", "SELL", "S"}:
        return fix.Side_SELL
    raise ValueError(f"Unsupported side: {side}")


def order_type_from_fix_ord_type(ord_type: str) -> OrderType:
    try:
        return FIX_ORD_TYPE_TO_VNPY_ORDER_TYPE[ord_type]
    except KeyError as exc:
        raise ValueError(f"Unsupported FIX OrdType: {ord_type}") from exc


def fix_ord_type_from_order_type(order_type: OrderType) -> str:
    try:
        return VNPY_ORDER_TYPE_TO_FIX_ORD_TYPE[order_type]
    except KeyError as exc:
        raise ValueError(f"Unsupported vn.py order type: {order_type}") from exc


def status_from_fix_ord_status(ord_status: str | None) -> Status:
    if ord_status is None:
        return Status.SUBMITTING

    try:
        return FIX_ORD_STATUS_TO_VNPY_STATUS[ord_status]
    except KeyError as exc:
        raise ValueError(f"Unsupported FIX OrdStatus: {ord_status}") from exc


def execution_report_from_fix(message: fix.Message) -> FixExecutionReport:
    cl_ord_id = get_field_value(message, fix.ClOrdID())
    symbol = get_field_value(message, fix.Symbol())
    if not cl_ord_id:
        raise ValueError("ExecutionReport missing ClOrdID")
    if not symbol:
        raise ValueError("ExecutionReport missing Symbol")

    return FixExecutionReport(
        order_id=get_field_value(message, fix.OrderID()),
        cl_ord_id=cl_ord_id,
        exec_id=get_field_value(message, fix.ExecID()),
        exec_type=get_field_value(message, fix.ExecType()),
        ord_status=get_field_value(message, fix.OrdStatus()),
        symbol=symbol,
        side=get_field_value(message, fix.Side()),
        order_qty=get_float_field(message, fix.OrderQty()),
        leaves_qty=get_float_field(message, fix.LeavesQty()),
        cum_qty=get_float_field(message, fix.CumQty()),
        avg_px=get_float_field(message, fix.AvgPx()),
        last_qty=get_last_qty(message),
        last_px=get_float_field(message, fix.LastPx()),
        text=get_field_value(message, fix.Text()),
    )


def order_data_from_execution_report(
    report: FixExecutionReport,
    gateway_name: str,
    exchange: Exchange = Exchange.LOCAL,
    order_type: OrderType = OrderType.LIMIT,
    offset: Offset = Offset.NONE,
    price: float = 0,
    reference: str = "",
    datetime: Datetime | None = None,
) -> OrderData:
    order_price = price or report.last_px or report.avg_px
    return OrderData(
        symbol=report.symbol,
        exchange=exchange,
        orderid=report.orderid,
        type=order_type,
        direction=report.direction,
        offset=offset,
        price=order_price,
        volume=report.order_qty,
        traded=report.cum_qty,
        status=report.status,
        datetime=datetime,
        reference=reference,
        gateway_name=gateway_name,
    )


def trade_data_from_execution_report(
    report: FixExecutionReport,
    gateway_name: str,
    exchange: Exchange = Exchange.LOCAL,
    offset: Offset = Offset.NONE,
    datetime: Datetime | None = None,
) -> TradeData | None:
    if not report.is_trade:
        return None

    return TradeData(
        symbol=report.symbol,
        exchange=exchange,
        orderid=report.orderid,
        tradeid=report.exec_id or f"{report.orderid}.{report.cum_qty}",
        direction=report.direction,
        offset=offset,
        price=report.last_px,
        volume=report.last_qty,
        datetime=datetime,
        gateway_name=gateway_name,
    )


def market_data_snapshot_from_fix(message: fix.Message) -> FixMarketDataSnapshot:
    symbol = get_field_value(message, fix.Symbol())
    if not symbol:
        raise ValueError("MarketDataSnapshotFullRefresh missing Symbol")

    md_req_id = get_field_value(message, fix.MDReqID())
    no_md_entries = int(get_field_value(message, fix.NoMDEntries(), 0))
    group = fix42.MarketDataSnapshotFullRefresh.NoMDEntries()

    bid_entries: list[tuple[float, float]] = []
    ask_entries: list[tuple[float, float]] = []
    last_price = 0.0
    last_volume = 0.0

    for index in range(1, no_md_entries + 1):
        message.getGroup(index, group)
        entry_type = get_field_value(group, fix.MDEntryType())
        entry_px = get_float_field(group, fix.MDEntryPx())
        entry_size = get_float_field(group, fix.MDEntrySize())

        if entry_type == fix.MDEntryType_BID:
            bid_entries.append((entry_px, entry_size))
        elif entry_type == fix.MDEntryType_OFFER:
            ask_entries.append((entry_px, entry_size))
        elif entry_type == fix.MDEntryType_TRADE:
            last_price = entry_px
            last_volume = entry_size

    bids = normalize_book_entries(bid_entries, reverse=True)
    asks = normalize_book_entries(ask_entries, reverse=False)
    values: dict[str, str | float | None] = {
        "symbol": symbol,
        "md_req_id": md_req_id,
        "last_price": last_price,
        "last_volume": last_volume,
    }
    for level in range(1, 6):
        bid_price, bid_volume = bids[level - 1]
        ask_price, ask_volume = asks[level - 1]
        values[f"bid_price_{level}"] = bid_price
        values[f"bid_volume_{level}"] = bid_volume
        values[f"ask_price_{level}"] = ask_price
        values[f"ask_volume_{level}"] = ask_volume
    return FixMarketDataSnapshot(**values)


def normalize_book_entries(
    entries: list[tuple[float, float]],
    *,
    reverse: bool,
) -> list[tuple[float, float]]:
    """Aggregate equal prices, sort best-to-worst, and pad to five levels."""

    volume_by_price: dict[float, float] = {}
    for price, volume in entries:
        if price <= 0 or volume < 0:
            continue
        volume_by_price[price] = volume_by_price.get(price, 0.0) + volume

    levels = sorted(volume_by_price.items(), reverse=reverse)[:5]
    levels.extend([(0.0, 0.0)] * (5 - len(levels)))
    return levels


def tick_data_from_market_data_snapshot(
    snapshot: FixMarketDataSnapshot,
    gateway_name: str,
    exchange: Exchange = Exchange.LOCAL,
    datetime: Datetime | None = None,
) -> TickData:
    values = dict(
        symbol=snapshot.symbol,
        exchange=exchange,
        datetime=datetime or Datetime.now(),
        last_price=snapshot.last_price,
        last_volume=snapshot.last_volume,
        gateway_name=gateway_name,
    )
    for level in range(1, 6):
        values[f"bid_price_{level}"] = getattr(snapshot, f"bid_price_{level}")
        values[f"bid_volume_{level}"] = getattr(snapshot, f"bid_volume_{level}")
        values[f"ask_price_{level}"] = getattr(snapshot, f"ask_price_{level}")
        values[f"ask_volume_{level}"] = getattr(snapshot, f"ask_volume_{level}")
    return TickData(**values)
