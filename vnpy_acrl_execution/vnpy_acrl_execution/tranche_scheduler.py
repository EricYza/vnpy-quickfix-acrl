from __future__ import annotations

from dataclasses import dataclass

from .ac_planner import AcPlan
from .constants import MODEL_INVENTORY
from .models import ParentOrder, Tranche, TrancheStatus


@dataclass(frozen=True)
class ScheduleCapacity:
    required_concurrency: int
    configured_concurrency: int

    @property
    def is_sufficient(self) -> bool:
        return self.configured_concurrency >= self.required_concurrency


class TrancheScheduler:
    def build_tranches(
        self,
        parent_id: str,
        plan: AcPlan,
        rl_horizon_seconds: float,
    ) -> list[Tranche]:
        if plan.total_volume % MODEL_INVENTORY:
            raise ValueError(
                f"total volume must be divisible by {MODEL_INVENTORY}"
            )

        tranches: list[Tranche] = []
        count = plan.total_volume // MODEL_INVENTORY
        for index in range(1, count + 1):
            milestone = index * MODEL_INVENTORY
            deadline_offset = plan.completion_time_for(milestone)
            planned_start = max(0.0, deadline_offset - rl_horizon_seconds)
            tranches.append(
                Tranche(
                    tranche_id=f"{parent_id}-T{index:04d}",
                    parent_id=parent_id,
                    sequence=index,
                    planned_start_offset=planned_start,
                    deadline_offset=deadline_offset,
                )
            )
        return tranches

    def due_to_start(self, parent: ParentOrder, now: float) -> list[Tranche]:
        slots = parent.request.max_active_tranches - len(parent.active_tranches)
        if slots <= 0:
            return []

        elapsed = now - parent.started_at
        waiting = [
            tranche
            for tranche in parent.tranches
            if tranche.status == TrancheStatus.WAITING
            and tranche.planned_start_offset <= elapsed
        ]
        waiting.sort(key=lambda tranche: (tranche.deadline_offset, tranche.sequence))
        return waiting[:slots]

    def capacity(self, plan: AcPlan, rl_horizon_seconds: float, configured: int) -> ScheduleCapacity:
        peak = 0
        tranches = self.build_tranches("CAPACITY", plan, rl_horizon_seconds)
        boundaries: list[tuple[float, int]] = []
        for tranche in tranches:
            boundaries.append((tranche.planned_start_offset, 1))
            boundaries.append((tranche.deadline_offset, -1))

        active = 0
        for _, delta in sorted(boundaries, key=lambda item: (item[0], item[1])):
            active += delta
            peak = max(peak, active)
        return ScheduleCapacity(peak, configured)

