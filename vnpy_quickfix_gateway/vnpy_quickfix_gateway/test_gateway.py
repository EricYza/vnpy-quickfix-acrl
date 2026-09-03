from __future__ import annotations

import argparse
import time
from datetime import UTC, datetime
from pathlib import Path

from vnpy.event import Event, EventEngine
from vnpy.trader.constant import Direction, Exchange, OrderType, Status
from vnpy.trader.event import EVENT_LOG, EVENT_ORDER, EVENT_TICK, EVENT_TRADE
from vnpy.trader.object import CancelRequest, LogData, OrderData, OrderRequest, TradeData
from vnpy.trader.object import SubscribeRequest, TickData

from vnpy_quickfix_gateway.gateway import DEFAULT_CONFIG, QuickfixGateway


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a minimal vn.py gateway test.")
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG,
        help="Path to the QuickFIX initiator config file.",
    )
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--symbol", default=None)
    parser.add_argument("--price", type=float, default=10.5)
    parser.add_argument("--quantity", type=float, default=100)
    parser.add_argument(
        "--cancel-test",
        action="store_true",
        help="Send one resting order and cancel it through gateway.cancel_order().",
    )
    parser.add_argument(
        "--market-data-test",
        action="store_true",
        help="Subscribe one symbol through gateway.subscribe().",
    )
    return parser.parse_args()


def print_log(event: Event) -> None:
    log: LogData = event.data
    print(f"[VNPY] log {log.gateway_name}: {log.msg}", flush=True)


def print_order(event: Event) -> None:
    order: OrderData = event.data
    print(
        f"[VNPY] order {order.vt_orderid} {order.symbol} "
        f"{order.direction} status={order.status} "
        f"volume={order.volume} traded={order.traded}",
        flush=True,
    )


def print_trade(event: Event) -> None:
    trade: TradeData = event.data
    print(
        f"[VNPY] trade {trade.vt_tradeid} order={trade.vt_orderid} "
        f"{trade.symbol} {trade.direction} volume={trade.volume} price={trade.price}",
        flush=True,
    )


def print_tick(event: Event) -> None:
    tick: TickData = event.data
    print(
        f"[VNPY] tick {tick.vt_symbol} last={tick.last_price}@{tick.last_volume} "
        f"bid1={tick.bid_price_1}@{tick.bid_volume_1} "
        f"ask1={tick.ask_price_1}@{tick.ask_volume_1}",
        flush=True,
    )


def wait_for_logon(gateway: QuickfixGateway, timeout: float = 10) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if gateway.application and gateway.application.logged_on:
            return True
        time.sleep(0.1)
    return False


def wait_for_order_status(
    gateway: QuickfixGateway,
    orderid: str,
    status: Status,
    timeout: float = 10,
) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        order = gateway.orders.get(orderid)
        if order and order.status == status:
            return True
        time.sleep(0.1)
    return False


def generate_symbol() -> str:
    return datetime.now(UTC).strftime("VNPY%H%M%S%f")


def send_matching_orders(gateway: QuickfixGateway, args: argparse.Namespace) -> None:
    symbol = args.symbol or generate_symbol()
    print(
        f"[TEST] gateway match symbol={symbol} quantity={args.quantity} price={args.price}",
        flush=True,
    )

    buy_req = OrderRequest(
        symbol=symbol,
        exchange=Exchange.LOCAL,
        direction=Direction.LONG,
        type=OrderType.LIMIT,
        volume=args.quantity,
        price=args.price,
    )
    sell_req = OrderRequest(
        symbol=symbol,
        exchange=Exchange.LOCAL,
        direction=Direction.SHORT,
        type=OrderType.LIMIT,
        volume=args.quantity,
        price=args.price,
    )

    buy_vt_orderid = gateway.send_order(buy_req)
    print(f"[TEST] sent vn.py BUY vt_orderid={buy_vt_orderid}", flush=True)
    time.sleep(0.2)

    sell_vt_orderid = gateway.send_order(sell_req)
    print(f"[TEST] sent vn.py SELL vt_orderid={sell_vt_orderid}", flush=True)


def run_cancel_test(gateway: QuickfixGateway, args: argparse.Namespace) -> None:
    symbol = args.symbol or generate_symbol()
    print(
        f"[TEST] gateway cancel symbol={symbol} quantity={args.quantity} price={args.price}",
        flush=True,
    )

    req = OrderRequest(
        symbol=symbol,
        exchange=Exchange.LOCAL,
        direction=Direction.LONG,
        type=OrderType.LIMIT,
        volume=args.quantity,
        price=args.price,
    )
    vt_orderid = gateway.send_order(req)
    orderid = vt_orderid.split(".", 1)[1]
    print(f"[TEST] sent vn.py BUY vt_orderid={vt_orderid}", flush=True)

    if not wait_for_order_status(gateway, orderid, Status.NOTTRADED):
        raise RuntimeError(f"Timed out waiting for NOTTRADED: {vt_orderid}")

    cancel_req = CancelRequest(
        orderid=orderid,
        symbol=symbol,
        exchange=Exchange.LOCAL,
    )
    gateway.cancel_order(cancel_req)
    print(f"[TEST] sent vn.py cancel orderid={orderid}", flush=True)

    if wait_for_order_status(gateway, orderid, Status.CANCELLED):
        print("[TEST] cancel test received CANCELLED order status", flush=True)
    else:
        print(
            f"[TEST] cancel test timed out; latest status="
            f"{gateway.orders[orderid].status}",
            flush=True,
        )


def run_market_data_test(gateway: QuickfixGateway, args: argparse.Namespace) -> None:
    symbol = args.symbol or generate_symbol()
    print(f"[TEST] gateway market data symbol={symbol}", flush=True)
    req = SubscribeRequest(symbol=symbol, exchange=Exchange.LOCAL)
    gateway.subscribe(req)
    print(f"[TEST] sent vn.py subscribe vt_symbol={req.vt_symbol}", flush=True)


def main() -> int:
    args = parse_args()
    event_engine = EventEngine()
    event_engine.register(EVENT_LOG, print_log)
    event_engine.register(EVENT_ORDER, print_order)
    event_engine.register(EVENT_TRADE, print_trade)
    event_engine.register(EVENT_TICK, print_tick)
    event_engine.start()

    gateway = QuickfixGateway(event_engine, QuickfixGateway.default_name)

    try:
        gateway.connect({"配置文件": str(args.config.expanduser().resolve())})
        if not wait_for_logon(gateway):
            raise RuntimeError("Timed out waiting for FIX Logon")

        if args.market_data_test:
            run_market_data_test(gateway, args)
        elif args.cancel_test:
            run_cancel_test(gateway, args)
        else:
            send_matching_orders(gateway, args)
        time.sleep(args.seconds)
    finally:
        gateway.close()
        event_engine.stop()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
