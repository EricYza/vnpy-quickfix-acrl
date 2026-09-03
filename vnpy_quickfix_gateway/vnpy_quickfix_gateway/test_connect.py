from __future__ import annotations

import argparse
import time
from datetime import UTC, datetime
from pathlib import Path

import quickfix as fix

from vnpy_quickfix_gateway.fix_application import FixApplication


DEFAULT_CONFIG = Path(__file__).with_name("quickfix_client.cfg")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the QuickFIX initiator smoke test.")
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG,
        help="Path to the QuickFIX initiator config file.",
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=0,
        help="Stop automatically after this many seconds. Use 0 to run until Ctrl+C.",
    )
    parser.add_argument(
        "--send-order",
        action="store_true",
        help="Send one FIX 4.2 NewOrderSingle after Logon.",
    )
    parser.add_argument("--symbol", default="AAPL", help="Order symbol.")
    parser.add_argument("--side", default="BUY", help="Order side: BUY/SELL/1/2.")
    parser.add_argument("--price", type=float, default=10.5, help="Limit price.")
    parser.add_argument("--quantity", type=float, default=100, help="Order quantity.")
    parser.add_argument("--cl-ord-id", default=None, help="Optional client order id.")
    parser.add_argument(
        "--match-test",
        action="store_true",
        help="Send a matching BUY/SELL pair after Logon.",
    )
    parser.add_argument(
        "--match-symbol",
        default=None,
        help="Symbol for --match-test. Defaults to a unique generated symbol.",
    )
    args = parser.parse_args()
    if args.send_order and args.match_test:
        parser.error("--send-order and --match-test cannot be used together")
    return args


def wait_for_logon(application: FixApplication, timeout: float = 10) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if application.logged_on:
            return True
        time.sleep(0.1)
    return False


def wait_for_execution_reports(
    application: FixApplication,
    count: int,
    timeout: float = 10,
) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if len(application.execution_reports) >= count:
            return True
        time.sleep(0.1)
    return False


def generate_match_symbol() -> str:
    return datetime.now(UTC).strftime("MATCH%H%M%S%f")


def run_match_test(application: FixApplication, args: argparse.Namespace) -> None:
    symbol = args.match_symbol or generate_match_symbol()
    buy_id = f"{application.generate_cl_ord_id()}B"
    sell_id = f"{application.generate_cl_ord_id()}S"

    print(
        f"[TEST] match test symbol={symbol} quantity={args.quantity} price={args.price}",
        flush=True,
    )
    application.send_new_order_single(
        symbol=symbol,
        side="BUY",
        price=args.price,
        quantity=args.quantity,
        cl_ord_id=buy_id,
    )
    wait_for_execution_reports(application, count=1, timeout=5)

    application.send_new_order_single(
        symbol=symbol,
        side="SELL",
        price=args.price,
        quantity=args.quantity,
        cl_ord_id=sell_id,
    )
    if wait_for_execution_reports(application, count=4, timeout=10):
        print("[TEST] match test received expected execution reports", flush=True)
    else:
        print(
            f"[TEST] match test timed out after "
            f"{len(application.execution_reports)} execution reports",
            flush=True,
        )


def main() -> int:
    args = parse_args()
    config_path = args.config.expanduser().resolve()

    settings = fix.SessionSettings(str(config_path))
    application = FixApplication()
    store_factory = fix.FileStoreFactory(settings)
    log_factory = fix.FileLogFactory(settings)

    initiator = fix.SocketInitiator(
        application,
        store_factory,
        settings,
        log_factory,
    )

    print(f"[TEST] using config: {config_path}", flush=True)
    initiator.start()
    print("[TEST] initiator started", flush=True)

    try:
        if args.send_order:
            if not wait_for_logon(application):
                raise RuntimeError("Timed out waiting for FIX Logon")
            application.send_new_order_single(
                symbol=args.symbol,
                side=args.side,
                price=args.price,
                quantity=args.quantity,
                cl_ord_id=args.cl_ord_id,
            )
        elif args.match_test:
            if not wait_for_logon(application):
                raise RuntimeError("Timed out waiting for FIX Logon")
            run_match_test(application, args)

        start_time = time.monotonic()
        while True:
            if args.seconds and time.monotonic() - start_time >= args.seconds:
                break
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("[TEST] stopping initiator", flush=True)
    finally:
        initiator.stop()
        print("[TEST] initiator stopped", flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
