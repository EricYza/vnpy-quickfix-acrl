# PPO Actor–Critic 训练的实际时间顺序与代码定位

本文按照项目实际运行顺序，说明一次市场决策如何产生状态、动作、奖励与下一状态，以及收集 400 条数据后 Actor 和 Critic 如何更新。

## 1. 当前训练任务

当前模型的执行任务为：

- 初始待卖库存：20 手；
- 截止时间：150 秒；
- 决策间隔：15 秒；
- 正常决策时刻：

$$
t=0,15,30,\ldots,135
$$

因此，一个完整回合最多包含 10 次有效决策。库存如果提前归零，回合会提前结束；如果到 150 秒仍有剩余库存，环境会将其强制市价卖出。

## 2. 单个市场回合里的实际时间顺序

### 2.1 在 $t=0$ 获得第一个状态

环境首先生成 67 维市场状态：

$$
S_0
$$

其中包含当前时间、剩余库存、订单簿深度、已有挂单、队列位置、订单流和价格变化等信息。

同一个 $S_0$ 同时输入 Actor 和 Critic：

$$
S_0
\begin{cases}
\xrightarrow{\mathrm{Actor}} A_0 \\
\xrightarrow{\mathrm{Critic}} V(S_0)
\end{cases}
$$

其中：

- $A_0$ 是当前立即执行的 7 维交易动作；
- $V(S_0)$ 是 Critic 对“从当前状态开始，按照当前策略继续交易直到回合结束”所能获得累计回报的预测。

Critic 不接收 $A_0$，也不是在预测某一个动作的即时收益。

### 2.2 环境执行 $A_0$

环境将 $A_0$ 转换为具体订单分配：

- 使用市价单卖出多少手；
- 在买一价上方第 1～5 档分别挂出多少手限价卖单；
- 多少库存暂时不交易。

随后，模拟市场从 $t=0$ 运行到 $t=15$。这段时间可能发生：

- 模型的市价单成交；
- 模型的限价单被动成交；
- NoiseAgent 和 StrategicAgent 产生新订单；
- 限价单被撤销、重新挂出或改变队列位置；
- 最优买卖价和订单簿深度变化。

环境将 0～15 秒期间所有成交产生的奖励相加，得到区间奖励：

$$
R_1
$$

并在 $t=15$ 生成新的 67 维状态：

$$
S_1
$$

因此，第一条强化学习转移数据为：

$$
(S_0,A_0,R_1,S_1)
$$

### 2.3 在 $t=15$ 再次决策

同一个 $S_1$ 再次同时进入 Actor 和 Critic：

$$
S_1
\begin{cases}
\xrightarrow{\mathrm{Actor}} A_1 \\
\xrightarrow{\mathrm{Critic}} V(S_1)
\end{cases}
$$

执行 $A_1$ 后，市场从 15 秒运行到 30 秒，产生：

$$
R_2,\quad S_2
$$

第二条转移数据为：

$$
(S_1,A_1,R_2,S_2)
$$

后续过程依次重复：

$$
(S_2,A_2,R_3,S_3),\ldots
$$

直到库存全部卖完或到达 150 秒。

### 2.4 最后一个决策与强制清仓

如果到 $t=135$ 仍有库存，模型进行最后一次正常决策：

$$
S_9
\begin{cases}
\xrightarrow{\mathrm{Actor}} A_9 \\
\xrightarrow{\mathrm{Critic}} V(S_9)
\end{cases}
$$

环境执行 $A_9$，然后从 135 秒继续运行到 150 秒。150 秒仍未成交的库存会被环境强制市价清仓。

135～150 秒期间的正常成交奖励和 150 秒的强制清仓奖励都会计入最后一个区间奖励：

$$
R_{10}
$$

最后一条转移可以表示为：

$$
(S_9,A_9,R_{10},S_{10}^{\mathrm{terminal}})
$$

因此，终端强制清仓产生的收益或损失会参与对 $A_9$ 以及更早动作的评价。

## 3. Critic 在每一步预测什么

在状态 $S_t$ 下，Critic 输出：

$$
V^{\pi}(S_t)
=
\mathbb{E}_{\pi}
\left[
R_{t+1}
+\gamma R_{t+2}
+\gamma^2R_{t+3}
+\cdots
\mid S_t
\right]
$$

它是对未来累计回报的预测，不是已经发生的实际奖励。

例如，在 $t=90$ 秒：

- 当前还剩 10 手；
- Actor 决定现在卖出其中 4 手；
- Critic 输出 $V(S_t)=0.8$。

这里的 0.8 表示：

> 从当前状态开始，如果继续按照当前策略交易到回合结束，预计还能获得 0.8 个模拟 tick 的累计收益。

它不代表卖出的 4 手获得了 0.8，也不代表下一步奖励一定是 0.8。

## 4. Critic 对应上一个动作还是当前动作

严格来说，Critic 输出不直接对应某个动作，因为当前项目使用的是状态价值函数：

$$
V(S_t)
$$

Critic 只接收状态，不接收动作。它在训练中同时承担两个作用：

1. 对当前动作 $A_t$ 而言，$V(S_t)$ 是执行动作前的预期回报基准；
2. 对上一个动作 $A_{t-1}$ 而言，$V(S_t)$ 是到达新状态后的后续价值估计。

上一个转移的 TD 误差为：

$$
\delta_{t-1}
=
R_t+\gamma V(S_t)-V(S_{t-1})
$$

当前转移的 TD 误差为：

$$
\delta_t
=
R_{t+1}+\gamma V(S_{t+1})-V(S_t)
$$

因此，$V(S_t)$ 对上一个动作来说是“进入当前状态后的价值”，对当前动作来说是“执行动作之前的预期基准”。这两种用途同时存在，但 Critic 始终没有输入 Actor 刚生成的动作。

## 5. Actor 和 Critic 不会每 15 秒立即更新

每一个决策时刻，Actor 和 Critic 只进行前向计算：

- Actor 根据 $S_t$ 生成动作 $A_t$；
- Critic 根据 $S_t$ 生成价值预测 $V(S_t)$；
- 环境执行动作并产生 $R_{t+1}$ 和 $S_{t+1}$；
- 上述数据暂时保存到经验数组中。

此时网络参数不会立即更新。

当前训练同时运行 4 个独立市场环境，每个环境连续收集 100 个决策步：

$$
4\ \text{个环境}
\times
100\ \text{个决策步}
=
400\ \text{条转移数据}
$$

一个决策步指一条：

$$
S_t\rightarrow A_t\rightarrow(R_{t+1},S_{t+1})
$$

它不是一个完整回合。由于一个完整回合最多约 10 个决策步，一个环境收集 100 步期间通常会经历多个回合；提前完成会使实际完成的回合数量发生变化。

收集完 400 条转移之后，才统一更新 Actor 和 Critic。

## 6. 收集 400 条数据后如何训练

### 6.1 从后向前计算 TD 误差

对每一条转移计算：

$$
\delta_t
=
R_{t+1}
+\gamma V(S_{t+1})
-V(S_t)
$$

如果回合已经终止，则没有下一状态价值，该项会被 `done` 掩码清零。

例如：

$$
V(S_t)=0.8,\qquad R_{t+1}=0.2,\qquad V(S_{t+1})=0.7
$$

且当前 $\gamma=1$，则：

$$
\delta_t=0.2+0.7-0.8=0.1
$$

这表示执行动作后的结果比 Critic 原先的预期好 0.1。

### 6.2 计算 GAE Advantage

代码将当前和后续 TD 误差组合为 Advantage：

$$
\hat{A}_t
=
\delta_t
+\gamma\lambda\delta_{t+1}
+(\gamma\lambda)^2\delta_{t+2}
+\cdots
$$

这里应区分：

- $A_t$：Actor 生成的交易动作；
- $\hat{A}_t$：训练中计算的 Advantage。

其含义为：

- $\hat{A}_t>0$：实际结果比 Critic 预期更好；
- $\hat{A}_t<0$：实际结果比 Critic 预期更差。

### 6.3 使用 PPO 更新 Actor

PPO 使用采样阶段保存的同一组 $(S_t,A_t)$，比较：

- 旧策略为动作 $A_t$ 给出的概率；
- 当前更新中策略为同一动作 $A_t$ 给出的概率。

如果 Advantage 为正，就提高这个动作的概率；如果 Advantage 为负，就降低这个动作的概率。PPO clipping 会限制单次策略改变的幅度。

### 6.4 更新 Critic

Critic 的回报目标为：

$$
\hat{G}_t
=
\hat{A}_t+V_{\mathrm{old}}(S_t)
$$

新的 Critic 对同一个旧状态 $S_t$ 重新计算：

$$
V_{\mathrm{new}}(S_t)
$$

价值损失为：

$$
L_V
=
\frac{1}{2}
\left(
V_{\mathrm{new}}(S_t)-\hat{G}_t
\right)^2
$$

代码变量 `new_value` 仍然对应同一个 $S_t$，不是 $S_{t+1}$。它之所以叫 `new_value`，只是因为它由更新过程中的当前网络参数重新计算。

## 7. 完整训练链路

```text
4 个市场环境同时运行
          ↓
每个环境读取当前 67 维状态 S_t
          ↓
Actor(S_t)  → 当前动作 A_t
Critic(S_t) → 状态价值 V(S_t)
          ↓
环境执行 A_t，并运行接下来的 15 秒
          ↓
得到区间奖励 R_{t+1} 和新状态 S_{t+1}
          ↓
保存 S_t、A_t、R_{t+1}、V(S_t) 和 done
          ↓
每个环境收集 100 步，共得到 400 条数据
          ↓
从后向前计算 TD 误差和 GAE Advantage
          ↓
使用 PPO Policy Loss 更新 Actor
使用 Value Loss 更新 Critic
          ↓
更新后的 Actor 和 Critic 采集下一批 400 条数据
```

## 8. 对应代码位置

### 8.1 Actor 与 Critic 网络结构

位置：[rl_files/ppo_logistic_normal.py](../rl_files/ppo_logistic_normal.py#L112)

Critic 定义：

```python
self.critic = nn.Sequential(...)
```

Actor 定义：

```python
self.actor_mean = nn.Sequential(...)
```

当前结构为：

$$
\mathrm{Actor}:\quad
67\rightarrow128\rightarrow128\rightarrow6
\rightarrow\mathrm{LogisticNormal}
\rightarrow7
$$

$$
\mathrm{Critic}:\quad
67\rightarrow128\rightarrow128\rightarrow1
$$

两个网络没有共享隐藏层，只是接收同一个状态。

### 8.2 同一个状态同时进入 Actor 和 Critic

位置：[rl_files/ppo_logistic_normal.py](../rl_files/ppo_logistic_normal.py#L150)

```python
action_mean = self.actor_mean(observation)
...
return action, log_probability, entropy, self.critic(observation)
```

其中 `observation` 就是同一个 $S_t$。

### 8.3 每一步生成动作和价值

位置：[rl_files/ppo_logistic_normal.py](../rl_files/ppo_logistic_normal.py#L493)

```python
action, log_probability, _, value = (
    agent.get_action_and_value(next_observation)
)
```

随后保存：

```python
values[step] = value.flatten()
actions[step] = action
log_probabilities[step] = log_probability
```

### 8.4 环境执行动作并返回奖励和新状态

位置：[rl_files/ppo_logistic_normal.py](../rl_files/ppo_logistic_normal.py#L506)

```python
next_observation_numpy,
reward,
terminations,
truncations,
infos = envs.step(action.cpu().numpy())
```

市场内部处理动作的位置：[simulation/market_gym.py](../simulation/market_gym.py#L220)

### 8.5 从后向前计算 TD 误差和 Advantage

位置：[rl_files/ppo_logistic_normal.py](../rl_files/ppo_logistic_normal.py#L565)

```python
delta = (
    rewards[step]
    + args.gamma * next_values * next_nonterminal
    - values[step]
)
```

随后计算 GAE：

```python
last_gae_lambda = (
    delta
    + args.gamma
    * args.gae_lambda
    * next_nonterminal
    * last_gae_lambda
)
```

### 8.6 PPO 更新 Actor

位置：[rl_files/ppo_logistic_normal.py](../rl_files/ppo_logistic_normal.py#L621)

这里使用同一个已保存的 $(S_t,A_t)$，重新计算当前策略下的动作概率，并在后续代码中计算 PPO Policy Loss。

### 8.7 更新 Critic

位置：[rl_files/ppo_logistic_normal.py](../rl_files/ppo_logistic_normal.py#L647)

```python
value_loss = 0.5 * (
    (new_value - batch_return_targets[minibatch_indices]) ** 2
).mean()
```

## 9. 一句话总结

> 在每个决策时刻，同一个 $S_t$ 同时进入 Actor 和 Critic；Actor 产生当前动作 $A_t$，Critic 产生动作执行前的累计回报预期 $V(S_t)$。环境执行 $A_t$ 后才产生 $R_{t+1}$ 和 $S_{t+1}$。Actor 和 Critic 不会每一步立即更新，而是在累计 400 条转移后，分别通过 PPO Policy Loss 和 Value Loss 统一训练。
