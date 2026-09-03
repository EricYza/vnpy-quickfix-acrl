from __future__ import annotations

from datetime import UTC, datetime
from math import sin

from vnpy.trader.constant import Exchange
from vnpy.trader.object import TickData


class SyntheticMarketFeed:
    """Deterministic five-level data for integration tests, not trading alpha."""

    def __init__(self) -> None:
        self._steps: dict[str, int] = {}

    def next_tick(
        self,
        symbol: str,
        exchange: Exchange,
        price_tick: float,
    ) -> TickData:
        vt_symbol = f"{symbol}.{exchange.value}"
        step = self._steps.get(vt_symbol, 0)
        self._steps[vt_symbol] = step + 1

        center = 100.0 + price_tick * 2 * sin(step / 7.0)
        best_bid = round((center - price_tick / 2.0) / price_tick) * price_tick
        best_ask = best_bid + price_tick
        bid_prices = [best_bid - index * price_tick for index in range(5)]
        ask_prices = [best_ask + index * price_tick for index in range(5)]
        bid_depths = [40 + ((step + index * 3) % 17) for index in range(5)]
        ask_depths = [38 + ((step * 2 + index * 5) % 19) for index in range(5)]

        values = {
            "symbol": symbol,
            "exchange": exchange,
            "datetime": datetime.now(UTC),
            "last_price": best_ask if step % 2 else best_bid,
            "last_volume": 1,
            "gateway_name": "ACRL_SIM",
        }
        for level in range(1, 6):
            values[f"bid_price_{level}"] = bid_prices[level - 1]
            values[f"ask_price_{level}"] = ask_prices[level - 1]
            values[f"bid_volume_{level}"] = bid_depths[level - 1]
            values[f"ask_volume_{level}"] = ask_depths[level - 1]
        return TickData(**values)

