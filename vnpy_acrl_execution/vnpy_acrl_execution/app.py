from pathlib import Path

from vnpy.trader.app import BaseApp

from .constants import APP_NAME
from .engine import AcRlExecutionEngine


class AcRlExecutionApp(BaseApp):
    app_name = APP_NAME
    app_module = "vnpy_acrl_execution"
    app_path = Path(__file__).parent
    display_name = "AC-RL Execution"
    engine_class = AcRlExecutionEngine
    widget_name = "AcRlManager"
    icon_name = ""

