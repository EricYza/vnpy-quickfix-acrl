from pathlib import Path


APP_NAME = "AC-RL Execution"
EVENT_ACRL_PARENT = "eAcRlParent"
EVENT_ACRL_TRANCHE = "eAcRlTranche"
EVENT_ACRL_INTENT = "eAcRlIntent"

MODEL_INVENTORY = 20
MODEL_DECISION_COUNT = 10
BOOK_LEVELS = 5
MODEL_CONTRACT_VERSION = "rlte-strategic-20-v1"
OBSERVATION_DIMENSION = 67
ACTION_DIMENSION = 7
EXPECTED_FEATURE_NAMES = (
    "normalized_time",
    "remaining_inventory_fraction",
    "best_bid_drift",
    "mid_price_drift",
    "normalized_spread",
    "book_imbalance",
    *(f"normalized_bid_depth_{level}" for level in range(1, 6)),
    *(f"normalized_ask_depth_{level}" for level in range(1, 6)),
    *(f"own_order_fraction_level_{level}" for level in range(1, 6)),
    "own_order_fraction_outside_levels",
    "own_order_fraction_inactive",
    *(f"inventory_unit_level_code_{unit}" for unit in range(1, 21)),
    *(f"inventory_unit_queue_code_{unit}" for unit in range(1, 21)),
    "recent_market_order_imbalance",
    "recent_limit_order_imbalance",
    "recent_cancellation_imbalance",
    "recent_mid_price_drift",
)
EXPECTED_ACTION_NAMES = (
    "market_sell_fraction",
    "limit_sell_level_1_fraction",
    "limit_sell_level_2_fraction",
    "limit_sell_level_3_fraction",
    "limit_sell_level_4_fraction",
    "limit_sell_level_5_fraction",
    "inactive_fraction",
)
DEFAULT_MODEL_SHA256 = (
    "7c108c25ab2f85335e87fc3bd85de51f"
    "0774128341784223ff593557fc645cf6"
)

PROJECTS_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RLTE_ROOT = PROJECTS_ROOT / "rlte"
DEFAULT_MODEL_PATH = (
    DEFAULT_RLTE_ROOT
    / "models"
    / (
        "strategic_20_seed_0_eval_seed_100_eval_episodes_1000_"
        "num_iterations_500_bsize_400_"
        "ppo_logistic_normal_comparison_200k.pt"
    )
)
DEFAULT_BOOK_SHAPE_PATH = (
    DEFAULT_RLTE_ROOT / "initial_shape" / "noise_flow_65.npz"
)

if len(EXPECTED_FEATURE_NAMES) != OBSERVATION_DIMENSION:
    raise RuntimeError("locked feature-name count does not match model dimension")
if len(EXPECTED_ACTION_NAMES) != ACTION_DIMENSION:
    raise RuntimeError("locked action-name count does not match model dimension")
