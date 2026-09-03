from __future__ import annotations

import argparse
from pathlib import Path

from vnpy.event import EventEngine
from vnpy.trader.engine import MainEngine
from vnpy.trader.ui import MainWindow, QtCore, create_qapp
from vnpy_quickfix_gateway.gateway import DEFAULT_CONFIG, QuickfixGateway

from .app import AcRlExecutionApp


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Start the normal VeighNa Trader UI with QuickFIX manual trading "
            "and the AC-RL execution app."
        )
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG,
        help="QuickFIX initiator configuration used by --connect.",
    )
    parser.add_argument(
        "--connect",
        action="store_true",
        help="Connect QUICKFIX automatically after the main window is shown.",
    )
    parser.add_argument(
        "--normal-window",
        action="store_true",
        help="Show a normal window instead of maximized.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    qapp = create_qapp("VeighNa AC-RL Trader")
    # The model has ten decisions per tranche; 10 Hz keeps scheduling error
    # bounded for short AC milestone windows while retaining vn.py's event path.
    event_engine = EventEngine(interval=0.1)
    main_engine = MainEngine(event_engine)
    main_engine.add_gateway(QuickfixGateway)
    main_engine.add_app(AcRlExecutionApp)

    main_window = MainWindow(main_engine, event_engine)
    if args.normal_window:
        main_window.show()
    else:
        main_window.showMaximized()

    if args.connect:
        config_path = str(args.config.expanduser().resolve())
        QtCore.QTimer.singleShot(
            0,
            lambda: main_engine.connect(
                {"配置文件": config_path},
                QuickfixGateway.default_name,
            ),
        )
    return qapp.exec()


if __name__ == "__main__":
    raise SystemExit(main())
