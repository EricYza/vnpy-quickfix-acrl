from __future__ import annotations

from dataclasses import dataclass
from math import sinh, sqrt

from .models import AcParameters


@dataclass(frozen=True)
class AcPlan:
    total_volume: int
    duration_seconds: float
    kappa: float

    def remaining_at(self, elapsed_seconds: float) -> float:
        t = min(max(elapsed_seconds, 0.0), self.duration_seconds)
        if self.kappa * self.duration_seconds < 1e-8:
            return self.total_volume * (1.0 - t / self.duration_seconds)

        numerator = sinh(self.kappa * (self.duration_seconds - t))
        denominator = sinh(self.kappa * self.duration_seconds)
        return self.total_volume * numerator / denominator

    def cumulative_at(self, elapsed_seconds: float) -> float:
        return self.total_volume - self.remaining_at(elapsed_seconds)

    def completion_time_for(self, cumulative_volume: float) -> float:
        target = min(max(cumulative_volume, 0.0), float(self.total_volume))
        if target <= 0:
            return 0.0
        if target >= self.total_volume:
            return self.duration_seconds

        low = 0.0
        high = self.duration_seconds
        for _ in range(80):
            midpoint = (low + high) / 2.0
            if self.cumulative_at(midpoint) < target:
                low = midpoint
            else:
                high = midpoint
        return (low + high) / 2.0


class AcPlanner:
    def create_plan(
        self,
        total_volume: int,
        duration_seconds: float,
        parameters: AcParameters,
    ) -> AcPlan:
        if total_volume <= 0:
            raise ValueError("total_volume must be positive")
        if duration_seconds <= 0:
            raise ValueError("duration_seconds must be positive")

        kappa = sqrt(
            parameters.risk_aversion
            * parameters.volatility
            * parameters.volatility
            / parameters.temporary_impact
        )
        return AcPlan(
            total_volume=total_volume,
            duration_seconds=duration_seconds,
            kappa=kappa,
        )

