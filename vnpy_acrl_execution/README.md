# vnpy_acrl_execution

`vnpy_acrl_execution` 是一个独立的 vn.py 执行算法 App。它把 Almgren-Chriss（AC）库存轨迹、固定 20 手的 RLTE 战术执行模型、中央订单协调器以及现有 `QuickfixGateway` 组合到同一个 VeighNa Trader 进程中。

本项目不会替换原来的 `vnpy_quickfix_gateway`。启动统一 Trader 后，两套入口同时存在：

- vn.py 主窗口原有 Trading 面板继续负责人工下单、人工撤单和人工行情订阅；
- “功能 -> AC-RL Execution”窗口负责 AC-RL 母单；
- 两者最终都通过同一个 `QuickfixGateway` 和 QuickFIX C++ SocketInitiator 发送 FIX 消息；
- 算法子单用 `OrderRequest.reference=ACRL:...` 标记，人工订单仍保持原有行为。

> 当前代码用于研究和链路验证，不是已经通过真实行情回放、仿真柜台或实盘验证的生产执行系统。

## 1. 系统结构

```text
VeighNa Trader 主窗口
├── 原 Trading 面板
│   ├── 人工限价单
│   ├── 人工撤单
│   └── 人工行情订阅
│
└── AC-RL Execution App
    ├── AcPlanner                 AC 累计成交目标
    ├── TrancheScheduler          每 20 手生成一个 RL tranche
    ├── MarketStateCache          行情和近期订单流状态
    ├── ObservationBuilder        严格构造 RLTE 67 维输入
    ├── RltePolicyAdapter         加载固定 67 -> 7 模型
    └── ExecutionCoordinator      reconciliation、风控、报撤单
             │
             ▼
        vn.py MainEngine
             │
             ▼
        QuickfixGateway
             │
             ▼
        QuickFIX Python/C++ 引擎
             │ SocketInitiator
             ▼
        FIX OrderMatch Server
```

## 2. AC 与 RL 如何协作

### 2.1 AC 只规划库存进度

用户输入母单总量和总期限。例如在 120 秒内卖出 260 手。`AcPlanner` 生成剩余库存轨迹：

```text
x(t) = X * sinh(kappa * (T - t)) / sinh(kappa * T)
```

其中：

- `X` 是母单总量；
- `T` 是母单期限；
- `kappa = sqrt(risk_aversion * volatility^2 / temporary_impact)`；
- `risk_aversion=0` 时退化为线性轨迹；
- 风险厌恶和波动率越大，计划通常越前置。

AC 不决定某一笔 FIX 子单的价格，也不直接调用 Gateway。

### 2.2 每个 AC 里程碑对应一个 20 手 RL tranche

当前 RL 权重只训练过固定 20 手任务，因此母单量必须是 20 的整数倍。260 手会被拆成 13 个 tranche：

```text
T0001: 累计成交目标达到 20 手的 AC 时刻
T0002: 累计成交目标达到 40 手的 AC 时刻
...
T0013: 累计成交目标达到 260 手的 AC 时刻
```

每个 tranche 都有独立的：

- 20 手初始库存；
- `started_at`；
- AC 里程碑对应的 `deadline_at`；
- 0 到 1 的归一化时间；
- 最多 10 次模型决策；
- 自身子单、成交量和剩余库存。

如果 tranche 的运行期限为 30 秒，正常决策间隔约为 3 秒；如果期限为 120 秒，间隔约为 12 秒。模型仍看到训练时相同的 10 个归一化进度点，而不是把现实秒数直接作为新特征。

### 2.3 中央协调器负责多 tranche 并发

多个 tranche 可以同时处于 ACTIVE，但它们不能各自无约束地直接发单。`ExecutionCoordinator` 是唯一允许把 RL 意图转成 vn.py 子单的组件，它负责：

- 限制同时活动的 tranche 数量；
- 将 7 个比例舍入为当前剩余库存的整数目标；
- 比较“目标挂单量”和“当前已确认/待撤子单量”；
- 先等待撤单确认，再释放这部分潜在库存；
- 跟踪部分成交和累计成交；
- 防止同一 tranche 或整个母单超卖；
- 将外部人工撤销算法子单视为异常并暂停母单；
- 拒单、FIX Logout、陈旧行情或不完整行情时暂停母单；
- tranche 到期后先撤被动单，再用最优买价的可成交限价单处理剩余量。

硬约束为：

```text
母单累计成交 + 所有可能成交的活动子单 <= 母单总量
tranche 累计成交 + 该 tranche 可能成交的子单 <= 20
```

## 3. 67 维模型输入契约

### 3.1 没有修改模型维度或字段语义

当前模型仍严格接收 RLTE `rlte-strategic-20-v1` 的 67 个 `float32`：

| 索引 | 数量 | 含义 |
|---:|---:|---|
| 0 | 1 | tranche 归一化时间 |
| 1 | 1 | 20 手中的剩余库存比例 |
| 2-4 | 3 | best bid 漂移、mid 漂移、spread |
| 5 | 1 | 五个连续 tick 桶的盘口不平衡 |
| 6-15 | 10 | 买卖侧五个连续 tick 桶的归一化深度 |
| 16-22 | 7 | 自身五档、范围外、未激活库存分布 |
| 23-42 | 20 | 每手库存的价格层编码 |
| 43-62 | 20 | 每手库存的队列位置编码 |
| 63-66 | 4 | 近期市价单、限价单、撤单不平衡和 mid 漂移 |

`tests/test_observation_contract.py` 会直接创建 RLTE 原始 `RLAgent` 和原始 `LimitOrderBook`，再把等价状态送入本项目的 `ObservationBuilder`，对 67 个元素逐项比较。目前单 tick spread 和宽 spread 两种情况都要求 `np.testing.assert_array_equal()`。

此外，策略加载边界会锁定 `rlte-strategic-20-v1` 的契约版本、67 个字段名及其顺序、7 个动作名及其顺序。任何字段新增、删除、改名或重排都会直接拒绝加载，不会在语义不一致时继续推理。默认模型文件还必须通过固定 SHA-256 校验。

### 3.2 `market_tick_size` 不是第 68 个特征

GUI 中的 `Market tick size (adapter only)` 是市场最小价格变动单位，例如 `0.01`。它不进入模型向量，也不改变任何槽位。

RLTE 仿真价格用整数 tick 表示，训练源码中的价格漂移公式为：

```text
(integer_tick_price - reference_integer_tick_price) / 10
```

现实报价若使用 `0.01`，等价适配为：

```text
(real_price - reference_real_price) / market_tick_size / 10
```

因此 `market_tick_size` 只是把现实价格换回训练时的 tick 单位。测试还验证了同一 tick 状态分别用整数价格和 `0.01` 报价表示时，得到的 67 维向量相同。

### 3.3 盘口是连续 tick 桶，不是简单的五个非空报价

RLTE 原始 `LimitOrderBook.level2()` 会保留 spread 内的空价格。例如 best bid 为 1000、best ask 为 1003 时，模型侧深度从对手盘边界连续取值：

```text
bid 模型桶: 1002, 1001, 1000, 999, 998
ask 模型桶: 1001, 1002, 1003, 1004, 1005
```

不存在订单的价格深度为 0。`MarketSnapshot.model_book_depths()` 会把 vn.py/FIX 的排名报价投影到这些连续 tick 桶，随后才进入未改变的 67 维构造。

### 3.4 保留训练时的历史差异

当前旧权重训练时存在一个历史语义差异，本项目没有擅自修正：

- 自身订单观察层以 `best_ask, best_ask+1 tick, ...` 为起点；
- RL 动作的限价目标以 `best_bid+1 tick, ..., best_bid+5 tick` 为起点。

spread 为 1 tick 时两者一致；spread 更宽时不完全一致。改变它需要重新训练模型，而不能只修改部署代码。

### 3.5 队列位置仍是估算值

训练环境拥有 Market-by-Order 信息，可知道每一手前方的精确数量。普通五档 Market-by-Price 行情只能看到聚合深度。本项目假设算法订单排在该价位外部数量之后，并按每手位置 `queue_ahead + 0, +1, ...` 编码；不会把超过 40 的位置裁剪。

这是当前接入真实市场的主要模型偏差之一。若以后能获得逐笔订单和逐笔撤单数据，应替换估算器，而不是改变 67 维字段定义。

## 4. 7 维输出和子单

模型输出 7 个非负比例，总和为 1：

```text
[market sell, limit L1, L2, L3, L4, L5, inactive]
```

这里的 market sell 在现有 Gateway 中实现为“卖价等于 best bid 的可成交 LIMIT”，因为当前 `QuickfixGateway` 只支持 LIMIT。L1-L5 的价格分别为：

```text
best_bid + 1 * tick_size
...
best_bid + 5 * tick_size
```

这些比例是目标库存分配，不是每次无条件新增的订单量。协调器会先做 reconciliation，避免每次模型调用都重复挂相同数量。

## 5. 原 Gateway 功能保留情况

统一入口仍创建 vn.py 原生 `MainWindow`，因此以下功能没有搬进算法窗口，也没有被替换：

- Trading 面板人工限价下单；
- 活动委托和全部委托监控；
- 人工撤单和 Cancel All；
- 行情订阅；
- 成交、订单和 Tick 回调显示；
- 系统菜单中的 `连接QUICKFIX`。

Gateway 本次只增加了兼容扩展：

- `QuickfixGateway.is_logged_on` 供算法风控读取；
- 保留 `OrderRequest.reference` 并在 ExecutionReport 转回 `OrderData` 时恢复；
- `MarketDataRequest` 请求深度从 1 改为 5；
- `MarketDataSnapshotFullRefresh` 最多解析并映射五个 bid/ask 报价；
- 原一档字段和人工订阅行为保持兼容。

## 6. 行情模式

### 6.1 Synthetic 模式

默认勾选 `Use deterministic synthetic 5-level market`。它在算法 App 内生成确定性的五档 Tick，仅用于验证：

```text
AC -> tranche -> 67 维 -> RL -> coordinator -> vn.py -> QuickFIX
```

Synthetic 行情不是外部 API、不是交易所行情，也不表示策略有效。算法子单仍会发到已登录的 QuickFIX server，因此运行前依然必须连接 server。

### 6.2 Gateway 行情模式

取消 synthetic 勾选后，算法通过 `MainEngine.subscribe()` 向 QuickFIX Gateway 订阅。模型调用前会检查：

- FIX 已 Logon；
- Tick 未超过 `max_market_age_seconds`；
- bid/ask 都有五个有序的非空报价；
- 能按 `market_tick_size` 投影到训练连续 tick 桶。

当前 QuickFIX ordermatch demo 只返回一次一档固定假快照 `10.0/10.5/10.25`，所以它可验证人工行情消息通道，但不满足 AC-RL 的真实行情模式。真实模式需要 server 持续发送完整五档 FullRefresh，或者后续实现标准 IncrementalRefresh 状态簿。

## 7. 安装

所有命令都在 WSL 中执行。不要手工覆盖 `PYTHONPATH`，因为 `vnpyfix` 的 conda 激活钩子会加入 QuickFIX Python 绑定路径。

```bash
cd /path/to/vnpy-quickfix-acrl
source ~/miniconda3/etc/profile.d/conda.sh
conda activate vnpyfix

python -m pip install -e ./vnpy_acrl_execution --no-deps --no-build-isolation
```

快速检查：

```bash
python -c "import quickfix, vnpy, vnpy_quickfix_gateway, vnpy_acrl_execution; print('imports ok')"
vnpy-acrl-logic-test
```

## 8. 启动与 GUI 操作

### 8.1 启动 QuickFIX ordermatch server

在第一个终端中：

```bash
source ~/miniconda3/etc/profile.d/conda.sh
conda activate vnpyfix
cd /path/to/vnpy-quickfix-acrl
./scripts/run_ordermatch.sh
```

### 8.2 启动统一 Trader 并自动连接

在第二个终端中：

```bash
source ~/miniconda3/etc/profile.d/conda.sh
conda activate vnpyfix
cd /path/to/vnpy-quickfix-acrl
./scripts/run_trader.sh --connect
```

也可以直接用模块入口：

```bash
python -m vnpy_acrl_execution.run_trader --connect
```

如果不使用 `--connect`，进入窗口后点击“系统 -> 连接QUICKFIX”，确认终端出现 `[FIX] onLogon`。

### 8.3 人工功能测试

仍在主窗口左侧 Trading 面板操作。例如：

```text
Exchange: LOCAL
Symbol: MANUAL01
Direction: Long 或 Short
Type: Limit
Price: 10.5
Volume: 100
Gateway: QUICKFIX
```

人工撤单可在活动委托上操作，或者点击 `Cancel All`。人工行情订阅 ordermatch 假行情时，应看到 `10.0@100 / 10.5@100 / last 10.25@10`；这只证明 FIX 行情通道畅通。

### 8.4 AC-RL synthetic 链路测试

1. 确认终端已经出现 `[FIX] onLogon`。
2. 点击“功能 -> AC-RL Execution”。
3. 保持 `Use deterministic synthetic 5-level market` 勾选。
4. 第一次建议勾选 `Use rule policy (logic test only)`，先隔离模型因素。
5. 输入 `Symbol=ACRLTEST`。
6. 输入 `Parent volume=20`、`Parent duration=30`、`RL tranche horizon=30`、`Max active=1`。
7. `Market tick size` 保持 `0.01`。
8. 点击 `Start parent`。
9. 在 Parent、Tranche、Intent 表格以及主窗口委托监控中观察子单。
10. 链路正确后取消 rule policy，使用真实 RLTE 权重再次运行。

测试 260 手并发调度时可使用默认参数：

```text
Parent volume: 260
Parent duration: 120 seconds
RL horizon: 30 seconds
Max active: 4
```

Capacity 必须显示 `(OK)`；否则窗口会拒绝启动母单。

## 9. 测试命令

AC-RL 单元与集成测试：

```bash
source ~/miniconda3/etc/profile.d/conda.sh
conda activate vnpyfix
cd /path/to/vnpy-quickfix-acrl
pytest -q vnpy_acrl_execution/tests
```

Gateway 五档和 reference 回归：

```bash
cd /path/to/vnpy-quickfix-acrl
pytest -q vnpy_quickfix_gateway/tests
```

只查看 AC tranche 计划，不连接 server：

```bash
vnpy-acrl-logic-test --volume 260 --seconds 120 --rl-horizon 30 --max-active 4
```

连接当前 ordermatch，自动准备对手盘并用真实模型完成一笔 20 手算法母单：

```bash
vnpy-acrl-fix-smoke
```

成功时最后会输出 `status=COMPLETED traded=20/20` 和 `[SMOKE] PASS`。该命令使用
synthetic 五档数据构造未改变的 67 维模型输入，但子单、成交和回报都真实经过
vn.py、QuickFIX SocketInitiator 和 ordermatch。

## 10. 文件职责

| 文件 | 职责 |
|---|---|
| `ac_planner.py` | AC 库存轨迹和累计成交里程碑反求 |
| `tranche_scheduler.py` | 创建 20 手 tranche、起止时间和并发容量 |
| `models.py` | 母单、tranche、意图、目标和状态数据结构 |
| `market_state.py` | Tick 缓存、近期流量、连续 tick 深度投影 |
| `observation.py` | 严格构造固定 67 维模型输入 |
| `policy_adapter.py` | RLTE 权重加载、哈希校验和 7 维推理 |
| `coordinator.py` | 子单 ownership、reconciliation 和硬风控 |
| `engine.py` | vn.py 事件注册、定时调度和组件编排 |
| `synthetic_market.py` | 确定性五档链路测试行情 |
| `app.py` | vn.py App 元数据 |
| `ui/widget.py` | AC-RL 参数、状态表和控制按钮 |
| `run_trader.py` | 原手动 UI + QuickFIX + AC-RL 的统一入口 |
| `fix_smoke.py` | 自动化真实 QuickFIX 端到端链路冒烟测试 |

## 11. 当前未完成与生产前要求

当前逻辑链路已经覆盖 AC 调度、并发 20 手策略、模型推理、订单 ownership、撤单确认、部分成交、终态乱序保护、硬数量上限、FIX Logout 和行情门禁，但仍有明确限制：

- ordermatch demo 行情仍是固定假数据且只有一档；
- Gateway 尚未实现 FIX `MarketDataIncrementalRefresh (35=X)` 本地订单簿；
- Market-by-Price 无法精确恢复每一手 FIFO 队列位置；
- 近期新增限价单/撤单方向由快照差分估算，不等于逐笔订单流；
- 当前模型只训练过卖出 20 手、150 仿真单位、每 15 单位决策一次；
- 任意现实期限虽然映射到归一化 10 次决策，但不代表模型已对该期限分布泛化；
- synthetic 模式只验证代码链路，不验证成交质量、市场冲击或收益；
- 没有真实历史 FIX 回放、手续费、延迟、拒单概率和断线恢复评估；
- 账户、持仓和交易所级风控仍由后续系统补充；
- 进程重启后的活动母单和子单状态恢复尚未实现。

进入仿真盘或实盘前，至少需要真实五档/逐笔行情回放、FIX 增量簿、订单恢复、kill switch、价格和数量限制、参与率限制、异常重连测试以及策略样本外评估。
