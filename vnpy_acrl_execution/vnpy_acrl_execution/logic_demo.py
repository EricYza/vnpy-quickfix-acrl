from __future__ import annotations

import argparse

from vnpy.trader.constant import Exchange

from .ac_planner import AcPlanner
from .models import AcParameters, ParentOrderRequest
from .tranche_scheduler import TrancheScheduler


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Print the AC-to-20-lot tranche schedule without a market server."
    )
    parser.add_argument("--volume", type=int, default=260)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--rl-horizon", type=float, default=30)
    parser.add_argument("--max-active", type=int, default=4)
    parser.add_argument("--volatility", type=float, default=0.02)
    parser.add_argument("--impact", type=float, default=0.1)
    parser.add_argument("--risk-aversion", type=float, default=0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    request = ParentOrderRequest(
        symbol="LOGIC",
        exchange=Exchange.LOCAL,
        total_volume=args.volume,
        duration_seconds=args.seconds,
        rl_horizon_seconds=args.rl_horizon,
        max_active_tranches=args.max_active,
        ac=AcParameters(
            volatility=args.volatility,
            temporary_impact=args.impact,
            risk_aversion=args.risk_aversion,
        ),
    )
    planner = AcPlanner()
    scheduler = TrancheScheduler()
    plan = planner.create_plan(
        request.total_volume,
        request.duration_seconds,
        request.ac,
    )
    capacity = scheduler.capacity(
        plan,
        request.rl_horizon_seconds,
        request.max_active_tranches,
    )
    tranches = scheduler.build_tranches("DEMO", plan, request.rl_horizon_seconds)

    print(
        f"AC plan volume={request.total_volume} duration={request.duration_seconds}s "
        f"kappa={plan.kappa:.8f}"
    )
    print(
        f"20-lot tranches={len(tranches)} required_concurrency="
        f"{capacity.required_concurrency} configured={capacity.configured_concurrency} "
        f"sufficient={capacity.is_sufficient}"
    )
    print("seq  planned_start  deadline  cumulative_target")
    for tranche in tranches:
        print(
            f"{tranche.sequence:>3}  {tranche.planned_start_offset:>13.3f}  "
            f"{tranche.deadline_offset:>8.3f}  {tranche.sequence * 20:>17}"
        )
    return 0 if capacity.is_sufficient else 2


if __name__ == "__main__":
    raise SystemExit(main())

