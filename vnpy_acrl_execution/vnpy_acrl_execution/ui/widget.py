from __future__ import annotations

from vnpy.event import Event, EventEngine
from vnpy.trader.constant import Exchange
from vnpy.trader.engine import MainEngine
from vnpy.trader.ui import QtCore, QtGui, QtWidgets

from ..constants import (
    APP_NAME,
    DEFAULT_MODEL_PATH,
    DEFAULT_RLTE_ROOT,
    EVENT_ACRL_INTENT,
    EVENT_ACRL_PARENT,
    EVENT_ACRL_TRANCHE,
)
from ..engine import AcRlExecutionEngine
from ..models import AcParameters, IntentRecord, ParentOrder, ParentOrderRequest, Tranche


class AcRlManager(QtWidgets.QWidget):
    parent_signal = QtCore.Signal(Event)
    tranche_signal = QtCore.Signal(Event)
    intent_signal = QtCore.Signal(Event)

    def __init__(self, main_engine: MainEngine, event_engine: EventEngine) -> None:
        super().__init__()
        self.main_engine = main_engine
        self.event_engine = event_engine
        engine = main_engine.get_engine(APP_NAME)
        if not isinstance(engine, AcRlExecutionEngine):
            raise RuntimeError(f"{APP_NAME} engine is not available")
        self.engine = engine
        self.parent_rows: dict[str, int] = {}
        self.tranche_rows: dict[str, int] = {}
        self._parent_event_handler = self.parent_signal.emit
        self._tranche_event_handler = self.tranche_signal.emit
        self._intent_event_handler = self.intent_signal.emit
        self._events_registered = False
        self.parent_signal.connect(self._process_parent_event)
        self.tranche_signal.connect(self._process_tranche_event)
        self.intent_signal.connect(self._process_intent_event)

        self.setWindowTitle("AC-RL Execution")
        self.resize(1280, 820)
        self._init_ui()
        self._register_events()
        self._refresh_capacity()

    def _init_ui(self) -> None:
        title = QtWidgets.QLabel("AC inventory curve / RL tactical execution")
        title_font = QtGui.QFont("DejaVu Serif", 16)
        title_font.setBold(True)
        title.setFont(title_font)
        title.setStyleSheet("color: #e8b04a; padding: 6px 0;")

        subtitle = QtWidgets.QLabel(
            "AC schedules cumulative inventory. Each RL tranche manages exactly "
            "20 lots and proposes market/level-1..5/inactive target allocation."
        )
        subtitle.setWordWrap(True)
        subtitle.setStyleSheet("color: #9fb3c8; padding-bottom: 8px;")

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignmentFlag.AlignRight)
        form.setFieldGrowthPolicy(QtWidgets.QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)

        self.symbol_edit = QtWidgets.QLineEdit("ACRLTEST")
        self.exchange_combo = QtWidgets.QComboBox()
        self.exchange_combo.addItem(Exchange.LOCAL.value)
        self.gateway_combo = QtWidgets.QComboBox()
        self.gateway_combo.addItems(self.main_engine.get_all_gateway_names())

        self.total_spin = QtWidgets.QSpinBox()
        self.total_spin.setRange(20, 1_000_000)
        self.total_spin.setSingleStep(20)
        self.total_spin.setValue(260)

        self.duration_spin = self._double_spin(1, 86_400, 120, 1)
        self.horizon_spin = self._double_spin(1, 86_400, 30, 1)
        self.max_active_spin = QtWidgets.QSpinBox()
        self.max_active_spin.setRange(1, 1_000)
        self.max_active_spin.setValue(4)
        self.price_tick_spin = self._double_spin(0.000001, 1_000_000, 0.01, 6)

        self.volatility_spin = self._double_spin(0, 1_000, 0.02, 6)
        self.impact_spin = self._double_spin(0.000001, 1_000, 0.1, 6)
        self.risk_spin = self._double_spin(0, 1_000, 0, 6)

        self.synthetic_check = QtWidgets.QCheckBox("Use deterministic synthetic 5-level market")
        self.synthetic_check.setChecked(True)
        self.rule_policy_check = QtWidgets.QCheckBox("Use rule policy (logic test only)")

        self.rlte_edit = QtWidgets.QLineEdit(str(DEFAULT_RLTE_ROOT))
        self.model_edit = QtWidgets.QLineEdit(str(DEFAULT_MODEL_PATH))
        self.capacity_label = QtWidgets.QLabel()
        self.capacity_label.setStyleSheet("font-weight: 600;")

        form.addRow("Gateway", self.gateway_combo)
        form.addRow("Exchange", self.exchange_combo)
        form.addRow("Symbol", self.symbol_edit)
        form.addRow("Parent volume", self.total_spin)
        form.addRow("Parent duration (s)", self.duration_spin)
        form.addRow("RL tranche horizon (s)", self.horizon_spin)
        form.addRow("Max active tranches", self.max_active_spin)
        form.addRow("Market tick size (adapter only)", self.price_tick_spin)
        form.addRow("AC volatility", self.volatility_spin)
        form.addRow("AC temporary impact", self.impact_spin)
        form.addRow("AC risk aversion", self.risk_spin)
        form.addRow("Market source", self.synthetic_check)
        form.addRow("Policy", self.rule_policy_check)
        form.addRow("RLTE root", self.rlte_edit)
        form.addRow("Model", self.model_edit)
        form.addRow("Capacity", self.capacity_label)

        self.start_button = QtWidgets.QPushButton("Start parent")
        self.pause_button = QtWidgets.QPushButton("Pause selected")
        self.resume_button = QtWidgets.QPushButton("Resume selected")
        self.cancel_button = QtWidgets.QPushButton("Cancel selected")
        self.start_button.setStyleSheet(
            "QPushButton { background: #176b63; color: white; font-weight: 700; padding: 8px; }"
            "QPushButton:hover { background: #21877c; }"
        )
        self.cancel_button.setStyleSheet(
            "QPushButton { background: #8b3f35; color: white; font-weight: 700; padding: 8px; }"
        )

        button_row = QtWidgets.QHBoxLayout()
        button_row.addWidget(self.start_button)
        button_row.addWidget(self.pause_button)
        button_row.addWidget(self.resume_button)
        button_row.addWidget(self.cancel_button)

        config_box = QtWidgets.QGroupBox("Parent order configuration")
        config_layout = QtWidgets.QVBoxLayout()
        config_layout.addLayout(form)
        config_layout.addLayout(button_row)
        config_box.setLayout(config_layout)
        config_box.setMaximumWidth(520)

        self.parent_table = self._create_table(
            [
                "Parent ID",
                "Symbol",
                "Total",
                "Traded",
                "Remaining",
                "Duration",
                "Status",
                "Message",
            ]
        )
        self.tranche_table = self._create_table(
            [
                "Tranche ID",
                "Parent",
                "Seq",
                "Start offset",
                "Deadline offset",
                "Decision",
                "Traded",
                "Remaining",
                "Status",
                "Message",
            ]
        )

        self.intent_text = QtWidgets.QPlainTextEdit()
        self.intent_text.setReadOnly(True)
        self.intent_text.setMaximumBlockCount(1_000)
        fixed_font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.SystemFont.FixedFont)
        self.intent_text.setFont(fixed_font)

        parent_box = QtWidgets.QGroupBox("Parent orders")
        parent_layout = QtWidgets.QVBoxLayout()
        parent_layout.addWidget(self.parent_table)
        parent_box.setLayout(parent_layout)

        tranche_box = QtWidgets.QGroupBox("20-lot RL tranches")
        tranche_layout = QtWidgets.QVBoxLayout()
        tranche_layout.addWidget(self.tranche_table)
        tranche_box.setLayout(tranche_layout)

        intent_box = QtWidgets.QGroupBox("RL decisions and reconciliation targets")
        intent_layout = QtWidgets.QVBoxLayout()
        intent_layout.addWidget(self.intent_text)
        intent_box.setLayout(intent_layout)

        right_splitter = QtWidgets.QSplitter(QtCore.Qt.Orientation.Vertical)
        right_splitter.addWidget(parent_box)
        right_splitter.addWidget(tranche_box)
        right_splitter.addWidget(intent_box)
        right_splitter.setSizes([190, 330, 180])

        content_splitter = QtWidgets.QSplitter(QtCore.Qt.Orientation.Horizontal)
        content_splitter.addWidget(config_box)
        content_splitter.addWidget(right_splitter)
        content_splitter.setSizes([470, 810])

        layout = QtWidgets.QVBoxLayout()
        layout.addWidget(title)
        layout.addWidget(subtitle)
        layout.addWidget(content_splitter)
        self.setLayout(layout)

        self.start_button.clicked.connect(self._start_parent)
        self.pause_button.clicked.connect(self._pause_parent)
        self.resume_button.clicked.connect(self._resume_parent)
        self.cancel_button.clicked.connect(self._cancel_parent)
        capacity_inputs = (
            self.total_spin,
            self.duration_spin,
            self.horizon_spin,
            self.max_active_spin,
            self.volatility_spin,
            self.impact_spin,
            self.risk_spin,
        )
        for widget in capacity_inputs:
            widget.valueChanged.connect(lambda _value: self._refresh_capacity())

    def _register_events(self) -> None:
        if self._events_registered:
            return
        self.event_engine.register(EVENT_ACRL_PARENT, self._parent_event_handler)
        self.event_engine.register(EVENT_ACRL_TRANCHE, self._tranche_event_handler)
        self.event_engine.register(EVENT_ACRL_INTENT, self._intent_event_handler)
        self._events_registered = True

    def _unregister_events(self) -> None:
        if not self._events_registered:
            return
        self.event_engine.unregister(EVENT_ACRL_PARENT, self._parent_event_handler)
        self.event_engine.unregister(EVENT_ACRL_TRANCHE, self._tranche_event_handler)
        self.event_engine.unregister(EVENT_ACRL_INTENT, self._intent_event_handler)
        self._events_registered = False

    def _request_from_form(self) -> ParentOrderRequest:
        total = self.total_spin.value()
        if total % 20:
            raise ValueError("Parent volume must be a multiple of 20")
        gateway = self.gateway_combo.currentText().strip()
        if not gateway:
            raise ValueError("No gateway is available")
        return ParentOrderRequest(
            symbol=self.symbol_edit.text().strip(),
            exchange=Exchange(self.exchange_combo.currentText()),
            total_volume=total,
            duration_seconds=self.duration_spin.value(),
            rl_horizon_seconds=self.horizon_spin.value(),
            max_active_tranches=self.max_active_spin.value(),
            gateway_name=gateway,
            price_tick=self.price_tick_spin.value(),
            use_synthetic_market=self.synthetic_check.isChecked(),
            ac=AcParameters(
                volatility=self.volatility_spin.value(),
                temporary_impact=self.impact_spin.value(),
                risk_aversion=self.risk_spin.value(),
            ),
        )

    def _start_parent(self) -> None:
        try:
            request = self._request_from_form()
            parent_id = self.engine.start_parent(
                request,
                rlte_root=self.rlte_edit.text().strip(),
                model_path=self.model_edit.text().strip(),
                use_rule_policy=self.rule_policy_check.isChecked(),
            )
            self.intent_text.appendPlainText(f"STARTED {parent_id}")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Cannot start parent", str(exc))

    def _pause_parent(self) -> None:
        self._run_selected(self.engine.pause_parent)

    def _resume_parent(self) -> None:
        self._run_selected(self.engine.resume_parent)

    def _cancel_parent(self) -> None:
        parent_id = self._selected_parent_id()
        if not parent_id:
            QtWidgets.QMessageBox.information(self, "Select parent", "Select one parent order first")
            return
        reply = QtWidgets.QMessageBox.question(
            self,
            "Cancel parent",
            f"Cancel {parent_id} and all of its active child orders?",
            QtWidgets.QMessageBox.StandardButton.Yes
            | QtWidgets.QMessageBox.StandardButton.No,
            QtWidgets.QMessageBox.StandardButton.No,
        )
        if reply == QtWidgets.QMessageBox.StandardButton.Yes:
            self._run_selected(self.engine.cancel_parent)

    def _run_selected(self, function) -> None:
        parent_id = self._selected_parent_id()
        if not parent_id:
            QtWidgets.QMessageBox.information(self, "Select parent", "Select one parent order first")
            return
        try:
            function(parent_id)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Operation failed", str(exc))

    def _refresh_capacity(self) -> None:
        try:
            request = self._request_from_form()
            capacity = self.engine.estimate_capacity(request)
            if capacity.is_sufficient:
                self.capacity_label.setText(
                    f"required={capacity.required_concurrency}, configured="
                    f"{capacity.configured_concurrency} (OK)"
                )
                self.capacity_label.setStyleSheet("color: #71c7a5; font-weight: 700;")
            else:
                self.capacity_label.setText(
                    f"required={capacity.required_concurrency}, configured="
                    f"{capacity.configured_concurrency} (insufficient)"
                )
                self.capacity_label.setStyleSheet("color: #ef8d77; font-weight: 700;")
        except Exception as exc:
            self.capacity_label.setText(str(exc))
            self.capacity_label.setStyleSheet("color: #ef8d77;")

    @QtCore.Slot(Event)
    def _process_parent_event(self, event: Event) -> None:
        parent: ParentOrder = event.data
        row = self.parent_rows.get(parent.parent_id)
        if row is None:
            row = self.parent_table.rowCount()
            self.parent_table.insertRow(row)
            self.parent_rows[parent.parent_id] = row
        values = [
            parent.parent_id,
            f"{parent.request.symbol}.{parent.request.exchange.value}",
            parent.request.total_volume,
            parent.traded,
            parent.remaining,
            f"{parent.request.duration_seconds:.1f}s",
            parent.status.value,
            parent.message,
        ]
        self._set_row(self.parent_table, row, values)

    @QtCore.Slot(Event)
    def _process_tranche_event(self, event: Event) -> None:
        tranche: Tranche = event.data
        row = self.tranche_rows.get(tranche.tranche_id)
        if row is None:
            row = self.tranche_table.rowCount()
            self.tranche_table.insertRow(row)
            self.tranche_rows[tranche.tranche_id] = row
        values = [
            tranche.tranche_id,
            tranche.parent_id,
            tranche.sequence,
            f"{tranche.planned_start_offset:.2f}",
            f"{tranche.deadline_offset:.2f}",
            f"{tranche.decision_index}/10",
            tranche.traded,
            tranche.remaining,
            tranche.status.value,
            tranche.message,
        ]
        self._set_row(self.tranche_table, row, values)

    @QtCore.Slot(Event)
    def _process_intent_event(self, event: Event) -> None:
        record: IntentRecord = event.data
        self.intent_text.appendPlainText(
            f"{record.tranche_id} t={record.normalized_time:.3f} "
            f"remaining={record.remaining} market={record.target.market_sell} "
            f"limits={record.target.limit_sell_levels} inactive={record.target.inactive}"
        )

    def _selected_parent_id(self) -> str:
        row = self.parent_table.currentRow()
        if row < 0:
            return ""
        item = self.parent_table.item(row, 0)
        return item.text() if item else ""

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:
        self._unregister_events()
        event.accept()

    def showEvent(self, event: QtGui.QShowEvent) -> None:
        # MainWindow caches app widgets, so a closed manager is shown again as
        # the same object rather than constructed from scratch.
        self._register_events()
        super().showEvent(event)

    @staticmethod
    def _create_table(headers: list[str]) -> QtWidgets.QTableWidget:
        table = QtWidgets.QTableWidget(0, len(headers))
        table.setHorizontalHeaderLabels(headers)
        table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectionBehavior.SelectRows)
        table.setSelectionMode(QtWidgets.QAbstractItemView.SelectionMode.SingleSelection)
        table.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
        table.setAlternatingRowColors(True)
        table.verticalHeader().setVisible(False)
        table.horizontalHeader().setStretchLastSection(True)
        table.horizontalHeader().setSectionResizeMode(
            QtWidgets.QHeaderView.ResizeMode.ResizeToContents
        )
        return table

    @staticmethod
    def _set_row(table: QtWidgets.QTableWidget, row: int, values: list[object]) -> None:
        for column, value in enumerate(values):
            item = table.item(row, column)
            if item is None:
                item = QtWidgets.QTableWidgetItem()
                table.setItem(row, column, item)
            item.setText(str(value))

    @staticmethod
    def _double_spin(
        minimum: float,
        maximum: float,
        value: float,
        decimals: int,
    ) -> QtWidgets.QDoubleSpinBox:
        spin = QtWidgets.QDoubleSpinBox()
        spin.setRange(minimum, maximum)
        spin.setDecimals(decimals)
        spin.setValue(value)
        return spin
