import numpy as np

from simulation.market_gym import Market


def make_environment() -> Market:
    return Market(
        {
            "seed": 100,
            "market_env": "strategic",
            "execution_agent": "rl_agent",
            "volume": 20,
            "terminal_time": 150,
            "time_delta": 15,
            "drop_feature": None,
        }
    )


def run_until_done(environment: Market, action: np.ndarray) -> dict:
    environment.reset()
    for _ in range(12):
        _, _, terminated, truncated, info = environment.step(action)
        assert not truncated
        if terminated:
            return info
    raise AssertionError("environment did not terminate")


def test_all_inventory_deferred_to_deadline_is_forced_liquidation():
    environment = make_environment()
    try:
        inactive_action = np.zeros(7, dtype=np.float32)
        inactive_action[-1] = 1.0

        info = run_until_done(environment, inactive_action)

        assert info["time"] == 150
        assert info["forced_liquidation_volume"] == 20
        assert info["forced_liquidation_rate"] == 1.0
        assert info["volume"] == 0
    finally:
        environment.close()


def test_immediate_market_sale_has_no_forced_liquidation():
    environment = make_environment()
    try:
        immediate_market_action = np.zeros(7, dtype=np.float32)
        immediate_market_action[0] = 1.0

        info = run_until_done(environment, immediate_market_action)

        assert info["time"] == 0
        assert info["forced_liquidation_volume"] == 0
        assert info["forced_liquidation_rate"] == 0.0
        assert info["volume"] == 0
    finally:
        environment.close()
