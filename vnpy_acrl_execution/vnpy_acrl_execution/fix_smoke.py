from __future__ import annotations

import argparse
from datetime import UTC, datetime
from pathlib import Path
from time import monotonic, sleep
from typing import Callable

from vnpy.event import EventEngine
from vnpy.trader.constant import Direction, Exchange, OrderType, Status
from vnpy.trader.engine import MainEngine
from vnpy.trader.object import CancelRequest, OrderRequest
from vnpy_quickfix_gateway.gateway import DEFAULT_CONFIG, QuickfixGateway

from .app import AcRlExecutionApp
from .engine import AcRlExecutionEngine
from .models import AcParameters, ParentOrderRequest, ParentStatus


ACTIVE_ORDER_STATUSES = {
    Status.SUBMITTING,
    Status.NOTTRADED,
    Status.PARTTRADED,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run one real MainEngine -> AC-RL -> QuickFIX -> ordermatch smoke test."
        )
    )
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--symbol", default="")
    parser.add_argument("--duration", type=float, default=3.0)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--rule-policy",
        action="store_true",
        help="Use the deterministic rule policy instead of the real RLTE weights.",
    )
    return parser.parse_args()


def wait_until(
    predicate: Callable[[], bool],
    timeout: float,
    description: str,
) -> None:
    deadline = monotonic() + timeout
    while monotonic() < deadline:
        if predicate():
            return
        sleep(0.05)
    raise TimeoutError(f"Timed out waiting for {description}")


def main() -> int:
    args = parse_args()
    if args.duration <= 0 or args.timeout <= 0:
        raise ValueError("duration and timeout must be positive")

    symbol = args.symbol or datetime.now(UTC).strftime("ACRLSMOKE%H%M%S%f")
    event_engine = EventEngine(interval=0.1)
    main_engine = MainEngine(event_engine)
    gateway = main_engine.add_gateway(QuickfixGateway)
    algorithm = main_engine.add_app(AcRlExecutionApp)
    if not isinstance(gateway, QuickfixGateway):
        raise RuntimeError("QuickFIX gateway registration failed")
    if not isinstance(algorithm, AcRlExecutionEngine):
        raise RuntimeError("AC-RL engine registration failed")

    parent_id = ""
    liquidity_orderid = ""
    try:
        main_engine.connect(
            {"配置文件": str(args.config.expanduser().resolve())},
            QuickfixGateway.default_name,
        )
        wait_until(lambda: gateway.is_logged_on, args.timeout, "FIX Logon")
        print(f"[SMOKE] FIX Logon complete; symbol={symbol}", flush=True)

        # The demo ordermatch has no external market.  This resting buy gives
        # the 20-lot sell algorithm deterministic server-side liquidity.
        liquidity_request = OrderRequest(
            symbol=symbol,
            exchange=Exchange.LOCAL,
            direction=Direction.LONG,
            type=OrderType.LIMIT,
            volume=20,
            price=101.0,
            reference="ACRL_SMOKE_LIQUIDITY",
        )
        liquidity_vt_orderid = main_engine.send_order(
            liquidity_request,
            QuickfixGateway.default_name,
        )
        liquidity_orderid = liquidity_vt_orderid.split(".", 1)[-1]
        wait_until(
            lambda: (
                liquidity_orderid in gateway.orders
                and gateway.orders[liquidity_orderid].status == Status.NOTTRADED
            ),
            args.timeout,
            "resting liquidity order acknowledgement",
        )
        print(
            f"[SMOKE] resting BUY accepted: {liquidity_vt_orderid} 20@101.0",
            flush=True,
        )

        request = ParentOrderRequest(
            symbol=symbol,
            exchange=Exchange.LOCAL,
            total_volume=20,
            duration_seconds=args.duration,
            rl_horizon_seconds=args.duration,
            max_active_tranches=1,
            price_tick=0.01,
            use_synthetic_market=True,
            ac=AcParameters(volatility=0.02, temporary_impact=0.1, risk_aversion=0),
        )
        parent_id = algorithm.start_parent(
            request,
            use_rule_policy=args.rule_policy,
        )
        print(
            f"[SMOKE] parent started: {parent_id} policy="
            f"{'RULE_TEST' if args.rule_policy else 'RLTE'}",
            flush=True,
        )
        wait_until(
            lambda: algorithm.parents[parent_id].status
            in {
                ParentStatus.COMPLETED,
                ParentStatus.PAUSED,
                ParentStatus.CANCELLED,
                ParentStatus.ERROR,
            },
            args.timeout,
            "parent terminal status",
        )

        parent = algorithm.parents[parent_id]
        print(
            f"[SMOKE] parent result: status={parent.status.value} "
            f"traded={parent.traded}/{parent.request.total_volume} "
            f"message={parent.message}",
            flush=True,
        )
        if parent.status != ParentStatus.COMPLETED or parent.traded != 20:
            raise RuntimeError(
                f"AC-RL smoke failed: status={parent.status.value}, "
                f"traded={parent.traded}, message={parent.message}"
            )
        print("[SMOKE] PASS", flush=True)
        return 0
    finally:
        if parent_id:
            parent = algorithm.parents.get(parent_id)
            if parent and parent.status in {ParentStatus.RUNNING, ParentStatus.PAUSED}:
                algorithm.cancel_parent(parent_id)

        if liquidity_orderid:
            liquidity = gateway.orders.get(liquidity_orderid)
            if liquidity and liquidity.status in ACTIVE_ORDER_STATUSES:
                main_engine.cancel_order(
                    CancelRequest(
                        orderid=liquidity_orderid,
                        symbol=symbol,
                        exchange=Exchange.LOCAL,
                    ),
                    QuickfixGateway.default_name,
                )
                sleep(0.2)
        main_engine.close()


if __name__ == "__main__":
    raise SystemExit(main())
