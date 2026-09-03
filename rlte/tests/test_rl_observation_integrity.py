import numpy as np

from limit_order_book.limit_order_book import LimitOrder, LimitOrderBook
from simulation.agents import RLAgent


def test_rl_observation_does_not_mutate_order_book_snapshots():
    lob = LimitOrderBook(list_of_agents=['initial_agent', 'rl_agent'], level=30)
    lob.process_order(LimitOrder('initial_agent', 'bid', 1000, 20, 0))
    lob.process_order(LimitOrder('initial_agent', 'ask', 1001, 20, 0))

    agent = RLAgent(
        action_book_levels=5,
        observation_book_levels=5,
        volume=2,
        terminal_time=150,
        start_time=0,
        time_delta=15,
        priority=0,
    )
    # RLAgent uses a scalar fallback when no shape file is supplied, while the
    # production environment always supplies an array from an npz file.
    agent.initial_shape = np.full(30, 20.0)

    bid_volumes_before = lob.data.bid_volumes[-1].copy()
    ask_volumes_before = lob.data.ask_volumes[-1].copy()

    observation = agent.get_observation(time=0, lob=lob)

    np.testing.assert_array_equal(lob.data.bid_volumes[-1], bid_volumes_before)
    np.testing.assert_array_equal(lob.data.ask_volumes[-1], ask_volumes_before)
    assert observation.shape == (agent.observation_space_length,)
