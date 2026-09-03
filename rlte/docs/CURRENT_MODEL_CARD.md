# 当前 Strategic PPO 模型卡

## 1. 模型身份

| 项目 | 值 |
|---|---|
| 用途 | 仿真限价订单簿中的卖出执行策略研究基线 |
| 算法 | PPO clipped surrogate + Actor–Critic |
| 市场环境 | `strategic` |
| 模型文件 | `models/strategic_20_seed_0_eval_seed_100_eval_episodes_1000_num_iterations_500_bsize_400_ppo_logistic_normal_comparison_200k.pt` |
| SHA-256 | `7c108c25ab2f85335e87fc3bd85de51f0774128341784223ff593557fc645cf6` |
| 文件大小 | 约 208 KiB |
| 参数量 | 51,335 |
| 输入 | 67 个 `float32` 特征 |
| 输出 | 7 个非负目标比例，总和为 1 |
| 奖励 | 整个回合的成交 VWAP 减去回合初始最优买价 |
| 当前状态 | 研究模型；未通过真实行情回放、仿真盘或实盘验证 |

使用模型前应核对 SHA-256，避免误加载名称相似但训练配置不同的检查点。
`models/` 目前被 `.gitignore` 忽略，因此仅提交代码不会自动携带模型文件；向另一项目或机器交接时必须单独复制该 `.pt`，随后重新核对哈希。

## 2. 固定任务定义

该模型只在以下任务分布上训练：

- 初始待卖库存：20 手；
- 截止时间：150 个仿真时间单位；
- 决策间隔：15 个仿真时间单位；
- 正常决策时刻：`0, 15, ..., 135`，最多 10 次；
- 到达 150 时，仿真执行层撤销剩余挂单并以市价卖出剩余库存；
- 市场由会响应订单簿状态的噪声订单流和额外 StrategicAgent 共同构成。

仿真时间单位尚未使用真实 FIX 行情校准，不能直接断言一个单位就是现实一秒。

## 3. 网络结构

Actor 与 Critic 不共享隐藏层。

Actor：

```text
67 → Linear(128) → Tanh → Linear(128) → Tanh → Linear(6)
```

Actor 输出 Logistic-Normal 的 6 个基础均值。确定性推理取这 6 个均值，并通过：

```text
Z = 1 + Σ exp(v_i)
a_i = exp(v_i) / Z, i=1..6
a_7 = 1 / Z
```

转换成 7 个总和为 1 的目标比例。

Critic：

```text
67 → Linear(128) → Tanh → Linear(128) → Tanh → Linear(1)
```

生产推理只需要 Actor；当前 `.pt` 同时保存了 Actor 与 Critic 权重，以保持和训练代码的检查点格式一致。

## 4. 训练配置

当前检查点对应的主要配置：

| 参数 | 值 |
|---|---:|
| 训练决策步 | 200,000 |
| 训练 seed | 0 |
| 并行环境数 | 4 |
| 每环境 rollout 步数 | 100 |
| 每批样本数 | 400 |
| 迭代数 | 500 |
| 学习率 | `5e-4` |
| PPO clip | `0.2` |
| minibatch 数 | 4 |
| 每批最多更新 epoch | 4 |
| `gamma` | 1.0 |
| `GAE lambda` | 1.0 |
| Critic 损失系数 | 0.5 |
| 熵损失系数 | 0.0 |
| 最大梯度范数 | 0.5 |
| target KL | 0.02 |
| 探索标准差日程 | 从 1.0 下降至 0.32 |

目前只有一个训练 seed，因此不能仅凭该检查点证明 PPO 算法普遍优于基线。

## 5. 已有评估结果

训练结束时的确定性评估（1,000 个 episode，评估 seed 从 100 开始）：

| 指标 | 值 |
|---|---:|
| mean reward | 1.0842 |
| std reward | 2.2991 |
| p05 / p50 / p95 | -1.95 / 0.025 / 4.30 |
| mean finish time | 104.6994 |
| mean passive fill rate | 0.0719 |

另有 200 个同 seed 配对基线评估，文件为：

- `rewards/strategic_20_ppo_baselines_seed_100_episodes_200_summary.csv`
- `rewards/strategic_20_ppo_baselines_seed_100_episodes_200_paired.csv`

该配对测试中 PPO 的 mean reward 为 1.0155。相对 Market-TWAP、SL、Linear-SL 的平均回报差分别约为 1.3715、2.8115、1.5698。这个结果只说明当前仿真任务上的表现，不等价于真实市场效果。

## 6. 推理方式

当前部署包装器位于 `deployment/strategic_policy.py`。推荐在 vn.py 的 Python 进程中进行 CPU 确定性推理：

```python
from deployment import StrategicExecutionPolicy

policy = StrategicExecutionPolicy.load(
    "models/strategic_20_seed_0_eval_seed_100_eval_episodes_1000_"
    "num_iterations_500_bsize_400_ppo_logistic_normal_comparison_200k.pt",
    device="cpu",
    expected_sha256=(
        "7c108c25ab2f85335e87fc3bd85de51f"
        "0774128341784223ff593557fc645cf6"
    ),
)

intent = policy.predict(observation_67)
allocation = intent.simulator_target_quantities(remaining_inventory=20)
```

`allocation` 只是复现仿真的整数目标分配，不是可直接发送的 FIX 指令。实际执行层必须结合已确认挂单、待撤单、部分成交和拒单状态进行 reconciliation。

在当前机器上，以 CPU 预热后循环调用 5,000 次零向量观察，测得单条推理平均约 0.14 ms。该数字只用于确认网络推理本身很轻，不代表 vn.py 状态构建、线程切换、风控、IPC 或 FIX 往返延迟，也不是生产性能保证。

## 7. 已知限制与禁止用途

### 固定库存维度

观察使用每一手库存的价格层和队列位置编码，因此维度为：

```text
27 + 2 × 初始库存
```

当前 20 手模型固定为 67 维，不能直接加载到 10 手的 47 维环境或 40 手的 107 维环境。

### 固定期限分布

模型只见过 150/15 的任务。虽然输入含归一化时间和剩余库存比例，但没有显式输入绝对总期限、剩余秒数或绝对初始数量，不能认为它能可靠处理任意截止时间。

### 行情特征要求高

模型依赖五档深度、近期新增限价单、撤单、成交方向以及自身队列位置。实际 FIX 行情是否能够提供这些信息仍需审计。

### 仿真—实盘差异

仿真尚未纳入或校准：

- 真实报单、撤单和行情延迟；
- FIX 消息乱序、丢失、重发及拒单；
- 手续费、返佣和税费；
- 交易所最小数量、最小价格变动和价格保护；
- 真实 Market-by-Price/Market-by-Order 队列机制；
- 真实交易量、参与率和自身市场冲击。

### 风控边界

模型不得直接发送 FIX 消息，也不得负责：

- 防止超卖；
- 订单状态机；
- 撤单/成交竞态处理；
- 截止强制清仓；
- 断线保护、行情陈旧检查和 kill switch。

这些必须由独立执行与风控层硬性保证。

## 8. 进入真实系统前的最低门槛

1. 完成 QuickFIX/vn.py 行情与交易能力审计；
2. 按实际可获得数据重新设计固定维度、可变库存/期限观察；
3. 用多任务和多个训练 seed 重新训练；
4. 加入延迟、手续费、拒单、部分成交与订单状态仿真；
5. 使用历史 FIX 日志做确定性回放；
6. 与 TWAP、SL、Linear-SL 等基线做样本外比较；
7. 完成 shadow mode 和仿真柜台验证；
8. 由执行层实施独立硬风控后，才考虑小规模真实交易。
