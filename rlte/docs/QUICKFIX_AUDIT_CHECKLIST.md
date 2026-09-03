# QuickFIX C++ / vn.py 接入能力审计清单

## 审计目标

在 QuickFIX 项目窗口中先只读检查，不接模型、不发真实订单。最终产物应是一张字段与能力矩阵，回答“当前 RL 状态能否由真实系统稳定构造，以及执行意图能否被安全落实”。

已知架构：

```text
QuickFIX 引擎：C++
前端/策略编排：vn.py（Python）
候选模型推理：PyTorch Python CPU
```

优先调查 C++ 与 vn.py 之间现有的绑定/进程边界，不要先假设模型需要进入 C++。

## 1. 组件与线程模型

- [ ] QuickFIX 的具体实现和版本：QuickFIX/C++、自研分支或其他；
- [ ] 使用的 FIX 版本及交易对手方数据字典；
- [ ] C++ 引擎是嵌入 Python、Python binding、独立进程，还是通过 IPC 通信；
- [ ] `fromApp`、`toApp`、`onLogon`、`onLogout` 的代码位置；
- [ ] 回调所在的线程及是否允许执行耗时逻辑；
- [ ] vn.py 收到行情和 ExecutionReport 的路径；
- [ ] C++ 到 Python 的队列、锁和背压机制；
- [ ] 时钟来源、时区和单调时钟使用情况；
- [ ] 是否已有母单/执行任务控制器。

## 2. 行情能力矩阵

对每项记录：FIX MsgType、tag/重复组、数据类型、单位、时间戳、是否快照、是否增量、丢失恢复方式。

- [ ] `MarketDataSnapshotFullRefresh` 支持情况；
- [ ] `MarketDataIncrementalRefresh` 支持情况；
- [ ] 最多盘口档数；
- [ ] Market-by-Price 还是 Market-by-Order；
- [ ] bid/ask 价格和数量；
- [ ] 新增、修改、删除动作是否可区分；
- [ ] 逐笔成交价格、数量和时间；
- [ ] 主动买卖方向是否直接提供或可靠推断；
- [ ] 撤单方向和数量是否可恢复；
- [ ] exchange timestamp 与 local receive timestamp；
- [ ] sequence number、增量断档检测和快照重建；
- [ ] 交易状态、停牌、集合竞价和涨跌停标志；
- [ ] tick size 和 price scale 来源；
- [ ] quantity 单位：股、手、张或合约。

需要明确回答：

| RL 特征 | 可直接获得 | 可推断 | 不可获得 | 误差/延迟 |
|---|---|---|---|---|
| 五档买卖价量 |  |  |  |  |
| 市价单不平衡 |  |  |  |  |
| 限价新增不平衡 |  |  |  |  |
| 撤单不平衡 |  |  |  |  |
| 自身订单价格层 |  |  |  |  |
| 自身前方队列量 |  |  |  |  |

## 3. 交易指令能力

- [ ] `NewOrderSingle` 支持的订单类型；
- [ ] 市价单在该市场是否真实支持，还是需要 marketable limit；
- [ ] `OrderCancelRequest`；
- [ ] `OrderCancelReplaceRequest`；
- [ ] TimeInForce 支持范围；
- [ ] 最小委托量、数量步长、最大单笔量；
- [ ] tick size、价格精度和价格保护；
- [ ] 涨跌停和动态价格笼子；
- [ ] 批量撤单或 session kill 能力；
- [ ] 下单速率、撤单速率和交易所限流；
- [ ] `ClOrdID` / `OrigClOrdID` 生成和持久化；
- [ ] 母单 ID 如何传递和关联子单；
- [ ] 多账户、多 session 和 routing 规则。

## 4. ExecutionReport 状态机

逐项定位代码和测试：

- [ ] PendingNew、New；
- [ ] PartiallyFilled、Filled；
- [ ] PendingCancel、Canceled；
- [ ] PendingReplace、Replaced；
- [ ] Rejected、Expired；
- [ ] OrderCancelReject；
- [ ] PossDup、重放和重复消息去重；
- [ ] CumQty、LeavesQty、LastQty、LastPx 的使用；
- [ ] 平均成交价的计算；
- [ ] 乱序 ExecutionReport 的处理；
- [ ] cancel/fill race；
- [ ] 本地状态与柜台状态不一致时的恢复；
- [ ] 断线期间成交的补发与 reconciliation。

需要给出当前订单状态的唯一真源：不能由模型自行维护一份互不一致的库存。

## 5. 会话与故障恢复

- [ ] FIX Session 配置和持久化 MessageStore；
- [ ] sequence reset、resend request、gap fill 行为；
- [ ] 登录失败和登出后的策略行为；
- [ ] 网络抖动和反复重连；
- [ ] C++ 进程崩溃后的订单恢复；
- [ ] Python/vn.py 进程崩溃后的任务恢复；
- [ ] C++ 正常但 Python 卡死时的 watchdog；
- [ ] 行情断流与交易通道正常的危险状态；
- [ ] 服务重启后活动订单查询或 mass status；
- [ ] 人工 kill switch 和全撤路径。

## 6. 性能测量

分别记录 p50/p95/p99：

- [ ] exchange timestamp → C++ receive；
- [ ] C++ receive → vn.py state update；
- [ ] 快照构建；
- [ ] PyTorch CPU 推理；
- [ ] reconciliation 和风控；
- [ ] vn.py → C++ send；
- [ ] C++ send → 柜台确认；
- [ ] 撤单请求 → 撤单确认。

同时记录峰值消息率、队列积压和订单簿重建耗时。

## 7. 风控与合规边界

- [ ] 防超卖约束的位置；
- [ ] 最大单笔数量和最大母单数量；
- [ ] 最大参与率；
- [ ] 最大价格偏离和滑点；
- [ ] 行情陈旧阈值；
- [ ] 临近截止时的执行升级规则；
- [ ] 模型超时、异常输出和加载失败的 fallback；
- [ ] 人工暂停、恢复和终止；
- [ ] 策略、模型版本和每次决策的审计日志；
- [ ] 实盘启用审批和账户白名单。

具体限制必须依据实际市场、券商和适用监管要求另行确认，不能从本仿真项目推断。

## 8. 测试设施

- [ ] QuickFIX unit/acceptance tests；
- [ ] 模拟柜台或交易所 certification 环境；
- [ ] 可重复的历史 FIX 日志；
- [ ] 行情和 ExecutionReport 回放器；
- [ ] 人工注入拒单、延迟、断线、乱序和重复消息；
- [ ] shadow mode：生成意图但禁止发单；
- [ ] paper/simulated trading；
- [ ] 每次决策完整重放测试；
- [ ] 与 TWAP/SL/Linear-SL 的同样本回放比较。

## 9. 审计完成后的必交产物

1. 架构图：C++ QuickFIX、Python/vn.py、行情缓存、订单状态机和发单路径；
2. FIX 消息和字段表；
3. RL 67 维字段可用性矩阵；
4. 订单状态机和恢复流程；
5. 延迟测量结果；
6. 风险缺口清单；
7. 推荐部署方式：vn.py 进程内 PyTorch、独立 Python 服务或 ONNX；
8. 对未来通用模型观察空间的约束建议。

只有拿到上述结果后，才回到本项目设计“可变数量 + 可变截止时间”的固定维度模型。

## 10. 可直接用于新 Codex 窗口的任务描述

```text
请先完整阅读这个 QuickFIX C++ + vn.py Python 项目，不要修改代码，也不要发送任何订单。

目标是未来接入一个强化学习卖出执行策略。模型应只输出目标订单分配，QuickFIX/vn.py 执行层负责订单状态、reconciliation、风控和 FIX 报文。目前候选研究模型需要五档盘口、近期成交/限价新增/撤单不平衡、自身活动订单和自身队列位置。

请按以下内容做能力审计：
1. 定位 C++ QuickFIX 与 vn.py 的进程、绑定、回调和线程边界；
2. 列出 FIX 版本、行情 MsgType、深度、快照/增量、Market-by-Price/Market-by-Order 及每个可用字段；
3. 判断五档深度、订单流不平衡和自身队列位置能否直接得到、推断或无法得到；
4. 定位 NewOrderSingle、Cancel、Replace、ExecutionReport、CancelReject 和 ClOrdID 状态管理；
5. 说明部分成交、撤单成交竞态、拒单、乱序、重复、断线重连和订单恢复逻辑；
6. 定位已有风控、母单控制器、测试柜台、历史 FIX 日志和回放能力；
7. 给出字段能力矩阵、订单状态机、架构图、风险缺口，以及建议采用 vn.py 进程内 PyTorch、独立 Python 服务还是 ONNX。

请明确引用代码文件和行号，并区分“代码已经实现”“协议可能支持”“当前无法确认”。
```
