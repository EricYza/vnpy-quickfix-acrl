from __future__ import annotations

import quickfix as fix
import quickfix42 as fix42
from vnpy.event import EventEngine
from vnpy.trader.constant import Direction, Exchange, Offset, OrderType, Status
from vnpy.trader.object import OrderRequest

from vnpy_quickfix_gateway.gateway import QuickfixGateway
from vnpy_quickfix_gateway.mapping import (
    FixExecutionReport,
    market_data_snapshot_from_fix,
    tick_data_from_market_data_snapshot,
)


def add_entry(message, entry_type: str, price: float, volume: float) -> None:
    group = fix42.MarketDataSnapshotFullRefresh.NoMDEntries()
    group.setField(fix.MDEntryType(entry_type))
    group.setField(fix.MDEntryPx(price))
    group.setField(fix.MDEntrySize(volume))
    message.addGroup(group)


def test_snapshot_maps_and_sorts_five_levels_without_breaking_level_one() -> None:
    message = fix42.MarketDataSnapshotFullRefresh()
    message.setField(fix.Symbol("BOOK5"))
    message.setField(fix.MDReqID("REQ1"))

    # Deliberately unsorted and with duplicate prices to exercise aggregation.
    for price, volume in [(99, 10), (100, 12), (98, 8), (97, 7), (96, 6), (95, 5), (100, 3)]:
        add_entry(message, fix.MDEntryType_BID, price, volume)
    for price, volume in [(103, 13), (101, 11), (102, 12), (105, 15), (104, 14), (106, 16)]:
        add_entry(message, fix.MDEntryType_OFFER, price, volume)
    add_entry(message, fix.MDEntryType_TRADE, 100.5, 4)

    snapshot = market_data_snapshot_from_fix(message)
    tick = tick_data_from_market_data_snapshot(snapshot, "QUICKFIX")

    assert [getattr(snapshot, f"bid_price_{level}") for level in range(1, 6)] == [100, 99, 98, 97, 96]
    assert [getattr(snapshot, f"ask_price_{level}") for level in range(1, 6)] == [101, 102, 103, 104, 105]
    assert snapshot.bid_volume_1 == 15
    assert snapshot.last_price == 100.5
    assert snapshot.last_volume == 4
    assert tick.bid_price_1 == 100
    assert tick.bid_price_5 == 96
    assert tick.ask_price_1 == 101
    assert tick.ask_price_5 == 105


def test_gateway_preserves_algorithm_reference_across_execution_report() -> None:
    gateway = QuickfixGateway(EventEngine(), "QUICKFIX")
    request = OrderRequest(
        symbol="REFTEST",
        exchange=Exchange.LOCAL,
        direction=Direction.SHORT,
        type=OrderType.LIMIT,
        volume=2,
        price=100,
        offset=Offset.NONE,
        reference="ACRL:P1:P1-T0001:L1",
    )

    vt_orderid = gateway.send_order(request)
    orderid = vt_orderid.split(".", 1)[1]
    report = FixExecutionReport(
        order_id=orderid,
        cl_ord_id=orderid,
        exec_id="EXEC1",
        exec_type=fix.ExecType_NEW,
        ord_status=fix.OrdStatus_NEW,
        symbol="REFTEST",
        side=fix.Side_SELL,
        order_qty=2,
        leaves_qty=2,
        cum_qty=0,
        avg_px=0,
        last_qty=0,
        last_px=0,
    )
    gateway.on_fix_execution_report(report)

    assert gateway.orders[orderid].status == Status.NOTTRADED
    assert gateway.orders[orderid].reference == request.reference
