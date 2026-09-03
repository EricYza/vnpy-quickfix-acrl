# QuickFIX 第三阶段导读：进入会话引擎

这份文档只做第三阶段的事情。

如果说：

- 第一阶段是在看“示例程序怎么用 QuickFIX”
- 第二阶段是在看“消息对象和协议层长什么样”

那么第三阶段就是第一次真正进入 QuickFIX 引擎内部。

这一阶段聚焦你指定的这些文件：

- `src/C++/Session.h`
- `src/C++/Session.cpp`
- `src/C++/SessionFactory.cpp`
- `src/C++/SessionSettings.cpp`
- `src/C++/DataDictionary.cpp`

为了把这几份源码讲通，我会补一点必要上下文：

- `src/C++/SessionState.h`
- `src/C++/SessionSettings.h`
- `src/C++/SessionFactory.h`
- `src/C++/DataDictionary.h`
- `src/C++/DataDictionaryProvider.h`
- `src/C++/Acceptor.cpp`
- `src/C++/Initiator.cpp`

原因很简单：

- `Session` 真正的状态标志很多都放在 `SessionState` 里
- `SessionSettings` 只是配置容器，真正把它们组装成 `Session` 的是 `SessionFactory`
- 配置里的每个 session 真正是由 `Acceptor` / `Initiator` 去遍历并创建的
- `DataDictionary.cpp` 负责协议校验，但 `Session` 才决定“在什么时候调用它”

这份文档专门回答你这一阶段的三个核心目标：

1. 会话状态怎么流转
2. 配置文件怎么变成运行时 session
3. 消息校验在什么阶段发生

---

## 1. 先给这一阶段一个总图

这一阶段最重要的不是先抠每一行代码，而是先建立一个运行时脑图。

QuickFIX 的会话引擎，你可以粗略理解成三层：

1. 配置装配层
   把 `.cfg` 读进来，变成 `SessionSettings`、`Dictionary`、`SessionID`，然后再创建 `Session`

2. 会话状态机层
   `Session` + `SessionState` 负责：
   - 登录
   - 心跳
   - 超时
   - 序号
   - 重传
   - 登出

3. 协议校验层
   `DataDictionary` 负责：
   - 这条消息是不是这个 FIX 版本
   - 这个 `MsgType` 合不合法
   - 字段格式和枚举值对不对
   - 必填字段缺没缺
   - repeating group 数量对不对

然后它们三层之间的关系是：

```text
配置文件
-> SessionSettings
-> Acceptor/Initiator 遍历每个 SessionID
-> SessionFactory::create(...)
-> Session 对象创建出来
-> 运行时收到字符串
-> Message 解析
-> DataDictionary 校验
-> Session 做会话级校验和状态推进
-> Application 回调
```

这就是第三阶段最核心的总线。

---

## 2. `Session` 到底是什么

如果你只记一句话：

> `Session` 是 QuickFIX 里“单个 FIX 会话”的总调度者。

它不是：

- socket 本身
- 纯消息容器
- 纯配置对象

它更像：

- 一个围绕单个 `SessionID` 运行的状态机

它需要同时知道：

- 我是谁，和谁通信
- 现在有没有登录
- 当前发送/接收序号是多少
- 什么时候该发 Logon
- 什么时候该发 Heartbeat
- 什么时候该发 TestRequest
- 什么时候该发 ResendRequest
- 收到消息后先校验什么，再推进什么，再回调应用

所以 `Session` 是一个非常“中心化”的类。

---

## 3. `Session.h` 先不要死读，要先分成几块看

`Session.h` 内容很多，但如果一上来逐行读，很容易被淹没。

更好的办法是先按职责分块。

### 3.1 第一块：对外可见的状态接口

这类函数是：

- `isEnabled()`
- `sentLogon()`
- `receivedLogon()`
- `isLoggedOn()`
- `isInitiator()`
- `isAcceptor()`
- `getExpectedSenderNum()`
- `getExpectedTargetNum()`

它们告诉你：

- `Session` 是个有明显状态的对象

### 3.2 第二块：发送和接收入口

关键函数是：

- `send(Message&)`
- `next(const UtcTimeStamp &now)`
- `next(const std::string &, const UtcTimeStamp &now, bool queued = false)`
- `next(const Message &, const UtcTimeStamp &now, bool queued = false)`

这四个函数特别重要，因为它们其实代表三种不同入口：

1. 定时器驱动入口
   `next(now)`

2. 原始网络字符串入口
   `next(rawString, now)`

3. 已解析消息入口
   `next(message, now)`

4. 出站入口
   `send(message)`

这一步非常关键。

因为它说明 `Session` 不是只有“收到消息时才工作”，它还有：

- 定时推进
- 主动发消息

### 3.3 第三块：协议消息专用处理函数

比如：

- `nextLogon(...)`
- `nextHeartbeat(...)`
- `nextTestRequest(...)`
- `nextLogout(...)`
- `nextReject(...)`
- `nextSequenceReset(...)`
- `nextResendRequest(...)`

这些函数说明 `Session` 是把 FIX 的 admin 协议消息显式拆开处理的。

也就是说：

- 它不是收到消息后全靠 `if/else` 写在一个函数里
- 而是每类 admin 行为都有自己的处理入口

### 3.4 第四块：自动生成 admin 消息的函数

比如：

- `generateLogon()`
- `generateResendRequest(...)`
- `generateSequenceReset(...)`
- `generateHeartbeat()`
- `generateTestRequest(...)`
- `generateReject(...)`
- `generateBusinessReject(...)`
- `generateLogout(...)`

这组函数说明：

- `Session` 不只是“被动收消息”
- 它也是 admin 协议消息的主要生产者

### 3.5 第五块：校验和异常处理相关

比如：

- `verify(...)`
- `validLogonState(...)`
- `doBadTime(...)`
- `doBadCompID(...)`
- `doTargetTooLow(...)`
- `doTargetTooHigh(...)`

这组函数说明：

- `Session` 负责的是“会话级校验”
- 而不是纯语法校验

这一点后面会和 `DataDictionary` 明确区分。

---

## 4. `SessionState`：真正记录状态位的地方

虽然第三阶段主角是 `Session`，但如果不看 `SessionState.h`，你会很难真正理解“状态流转”。

### 4.1 `SessionState` 是什么

它的注释写得很直白：

> Maintains all of state for the Session class.

也就是：

- 它是 `Session` 的状态存储仓库

### 4.2 最重要的几个布尔状态位

`SessionState` 里最关键的几个标志是：

- `m_enabled`
- `m_receivedLogon`
- `m_sentLogout`
- `m_sentLogon`
- `m_sentReset`
- `m_receivedReset`
- `m_initiate`

你可以先把它们翻译成人话：

- `enabled`
  这个 session 当前是不是被允许运行

- `receivedLogon`
  我是否已经收到对方的 Logon

- `sentLogon`
  我是否已经发出过 Logon

- `sentLogout`
  我是否已经发出过 Logout

- `sentReset` / `receivedReset`
  ResetSeqNumFlag 的交互状态

- `initiate`
  我是 initiator 还是 acceptor

### 4.3 它不只记布尔值，还记时钟

还很重要的成员有：

- `m_lastSentTime`
- `m_lastReceivedTime`
- `m_heartBtInt`
- `m_logonTimeout`
- `m_logoutTimeout`
- `m_testRequest`

这些共同决定：

- 什么时候该发心跳
- 什么时候该发 test request
- 什么时候认定超时

### 4.4 它还管理序号和持久化

`SessionState` 继承了：

- `MessageStore`
- `Log`

同时内部持有：

- `MessageStore *m_pStore`
- `Log *m_pLog`

所以它其实还代管：

- 下一个 sender seq num
- 下一个 target seq num
- 消息持久化
- 日志输出

也就是说：

> `SessionState` 不只是几位状态标志，它还是 session 的状态后端。

### 4.5 它还带消息队列

`SessionState` 里还有：

- `Messages m_queue`
- `ResendRange m_resendRange`

这说明当 QuickFIX 遇到：

- 对端消息序号太高
- 需要等重传补齐缺口

时，它会先把消息挂起来。

这一步对理解 “高序号消息为什么不会立刻交给应用层” 非常重要。

---

## 5. 会话状态是怎么流转的

现在可以开始回答第一个核心问题了。

### 5.1 先给一个简化版状态图

一个 session 的典型流转可以先粗略看成：

```text
Created
-> Waiting for Logon
-> Sent Logon / Received Logon
-> Logged On
-> Normal traffic
-> Logout / Disconnect
-> Reset or idle
```

这只是简图，真正源码里会更细，但先有这张图会好很多。

### 5.2 Session 创建时发生什么

在 `Session` 构造函数里，会做这些事：

1. 初始化一堆默认参数
   - `m_sendRedundantResendRequests = false`
   - `m_checkCompId = true`
   - `m_checkLatency = true`
   - `m_maxLatency = 120`
   - `m_persistMessages = true`
   - 等等

2. 把 `heartBtInt` 写进 `m_state`

3. 根据 `heartBtInt != 0` 判断自己是不是 initiator

4. 创建消息存储
   `m_state.store(m_messageStoreFactory.create(...))`

5. 创建日志对象

6. 如果当前时间不在 session time 内，就 reset store

7. 把自己注册进全局 session 表

8. 调应用层 `onCreate`

9. 记录事件 `Created session`

这一步很重要，因为它说明：

- `Session` 对象一创建出来，就已经挂上 store 和 log 了
- 它不是“第一次收消息时才准备好”

### 5.3 为什么 `heartBtInt != 0` 会被用来区分 initiator / acceptor

在 `SessionFactory::create()` 里：

- initiator 会从配置读 `HeartBtInt`
- acceptor 则先给 0

然后在 `Session` 构造函数中：

```cpp
m_state.initiate(heartBtInt != 0);
```

这意味着：

- initiator 在创建时就知道自己的心跳间隔
- acceptor 要等收到对方 Logon 后，再从消息里拿到 `HeartBtInt`

这是一个很细但很关键的设计点。

### 5.4 定时器驱动的主状态推进：`Session::next(const UtcTimeStamp &now)`

这是整个会话状态机的“定时心跳函数”。

它做的逻辑大致是：

1. 如果当前不在 session time，reset 并返回
2. 如果 session disabled 或不在 logon time：
   - 如果已登录，发 Logout
   - 如果未登录，直接返回
3. 如果还没收到 Logon：
   - initiator 在合适时间发 Logon
   - 如果等 Logon 等太久，则断开
4. 如果已经登录但 `heartBtInt == 0`，返回
5. 检查 logout timeout
6. 检查是否仍在 heartbeat 窗口内
7. 如果对端长时间没消息，判定超时并断开
8. 否则：
   - 需要时发 TestRequest
   - 否则需要时发 Heartbeat

这说明：

> `next(now)` 是“时间驱动的会话维护循环”。

### 5.5 登录状态推进：`nextLogon(...)`

`nextLogon()` 是理解 QuickFIX 登录流程最关键的函数之一。

它做的事情很多，但可以按顺序理解：

1. 确保 Header 里有 `SenderCompID` / `TargetCompID`
2. 如果 `RefreshOnLogon=Y`，先 refresh store
3. 如果 session disabled，直接断开
4. 如果 logon 时间不合法，断开
5. 读取 `ResetSeqNumFlag`
6. 如果收到 reset 标记：
   - 记录状态
   - 必要时 reset store
7. 如果自己本来就应该主动发 logon，却先收到了异常 logon 响应，断开
8. acceptor 且 `ResetOnLogon=Y` 时 reset store
9. 调 `verify(logon, false, true)`
10. 标记 `receivedLogon(true)`
11. 处理 `NextExpectedMsgSeqNum`
12. 如果我是 acceptor：
   - 取对方的 `HeartBtInt`
   - 生成 logon response
13. 处理高序号 logon 的排队或 resend
14. 当双方都完成 logon 后，调应用层 `onLogon`

你可以看出来：

- 登录不是“只收一个 A 消息然后把布尔值改成 true”
- 它同时牵涉：
  - 时间窗
  - reset 语义
  - 心跳间隔
  - 序号同步
  - 可能的重传

### 5.6 正常工作态：已登录后的周期行为

当 `receivedLogon` 和 `sentLogon` 都为真时：

- `isLoggedOn()` 就成立

这之后会话进入“正常运行态”，此时主要由：

- `next(now)` 负责定时维护
- `next(message, now)` 负责处理入站消息
- `sendRaw(...)` 负责出站消息

在这个阶段，QuickFIX 主要做的是：

- 更新序号
- 检查心跳
- 发 test request
- 处理 resend / sequence reset
- 调用应用回调

### 5.7 断开与登出：`disconnect()`

`disconnect()` 很值得单独记住。

它会做：

1. 如果有 responder，先通知 `Disconnecting`
2. 断开网络 responder
3. 如果 session 处于 logon 相关状态：
   - 清 `receivedLogon`
   - 清 `sentLogon`
   - 调应用层 `onLogout`
4. 清：
   - `sentLogout`
   - `receivedReset`
   - `sentReset`
   - queue
   - resendRange
5. 如果 `ResetOnDisconnect=Y`，reset store

这意味着：

- `disconnect()` 不只是“关 socket”
- 它会同时清理一整套会话状态

### 5.8 一个更贴近源码的状态理解方式

与其把它想成那种非常严格的有限状态机图，不如更贴近源码一点地理解为：

- `Session` 通过一组布尔值、时钟值、序号和队列来表达状态
- 然后在 `next(now)`、`nextLogon()`、`verify()`、`disconnect()` 这些函数里推进这些状态

也就是说：

- QuickFIX 这套 session 引擎是“状态变量驱动”
- 而不是“显式 enum state 驱动”

---

## 6. 出站消息时，会话状态怎么变化

第三阶段很容易只盯着收消息，但出站消息也很关键。

### 6.1 `send(Message&)` 很薄

`send(Message&)` 做的事情不多：

1. 去掉 `PossDupFlag`
2. 去掉 `OrigSendingTime`
3. 调 `sendRaw(message)`

这表示：

- 真实出站逻辑都在 `sendRaw`

### 6.2 `sendRaw(...)` 做了哪些关键事情

`sendRaw()` 可以说是“发消息时的总装配线”。

核心流程是：

1. 拿到消息 Header
2. 取 `MsgType`
3. 调 `fill(header)` 自动补：
   - BeginString
   - SenderCompID
   - TargetCompID
   - MsgSeqNum
   - SendingTime
4. 如果传入了显式序号，则覆盖 `MsgSeqNum`
5. 如果是 admin message：
   - 回调 `toAdmin`
   - 特殊处理 Logon + ResetSeqNumFlag
   - `toString()`
   - `persist(...)`
   - 合法时真正发送
6. 如果是 app message：
   - 若未登录且即将 reset，直接不发
   - 回调 `toApp`
   - `toString()`
   - `persist(...)`
   - 已登录时真正发送

### 6.3 `fill(header)` 是所有出站消息的公共填充

`fill(...)` 会设置：

- `BeginString`
- `SenderCompID`
- `TargetCompID`
- `MsgSeqNum`
- `SendingTime`

这意味着无论你在应用层构造消息时填没填这些字段，session 层都会统一补齐。

### 6.4 `persist(...)` 很关键

`persist(...)` 做了两件事：

1. 如果 `PersistMessages=Y`，把消息字符串存入 store
2. `incrNextSenderMsgSeqNum()`

这意味着：

- sender seq num 的推进，是在消息被持久化后进行的

所以你可以把它理解成：

- “准备发送并记录成功后，发送序号往前走一步”

### 6.5 `toAdmin` / `toApp` 在会话引擎里的位置

这是你从第一阶段延续到第三阶段后会突然更清楚的一点。

在发送路径里：

- `Session` 先做会话级 header 填充
- 然后把消息交给你的 `Application::toAdmin()` 或 `Application::toApp()`

所以应用层是在：

- 协议会话层已经基本准备好消息之后
- 但真正发出去之前

拿到最后的修改机会。

---

## 7. 入站消息时，会话引擎到底走几层

这是第三阶段最关键的第二条主线。

不要把“收到消息”想成一个步骤，它其实至少分三层。

### 7.1 第一层：原始字符串入口 `next(const std::string&, now)`

这个函数做的事情大意是：

1. 记录 incoming 日志
2. 取 session data dictionary
3. 如果是 FIXT：
   - 再取 application data dictionary
4. 用这些字典构造 `Message(...)`
5. 再调用 `next(const Message&, now, queued)`

这一步非常重要，因为它说明：

> 收到网络字符串后，QuickFIX 不是马上进入应用层，而是先把字符串解析成 `Message`。

### 7.2 这里就已经有第一层校验了

注意它构造 `Message` 时传了：

- `sessionDD`
- `applicationDD`
- `m_validateLengthAndChecksum`

这意味着解析阶段已经会做一部分工作，例如：

- 消息字符串是否能正常解析
- 长度和校验和是否要检查

所以“消息校验”并不只发生在 `DataDictionary::validate()` 这一处。

### 7.3 第二层：已解析消息入口 `next(const Message&, now)`

这个函数才是 session 引擎处理入站消息的真正主战场。

它的顺序非常关键：

1. 检查 session time
2. 从 Header 取出：
   - `MsgType`
   - `BeginString`
   - `SenderCompID`
   - `TargetCompID`
3. 检查 BeginString 是否与当前 session 匹配
4. 如果是 Logon，设置目标端默认应用版本
5. 选择合适的 dictionary
6. 调 `DataDictionary::validate(...)`
7. 按 `MsgType` 分派到：
   - `nextLogon`
   - `nextHeartbeat`
   - `nextTestRequest`
   - `nextSequenceReset`
   - `nextLogout`
   - `nextResendRequest`
   - `nextReject`
   - 或普通消息路径
8. 普通消息则进一步走 `verify(...)`
9. 必要时推进 target seq num
10. 各类异常会被转换成 Reject / BusinessReject / Logout / disconnect

这条顺序线非常非常重要。

### 7.4 第三层：`verify(...)` 做会话级校验

`verify(...)` 做的是：

1. 看 `MsgType` 的 logon 状态是否合法
2. 检查 `SendingTime` 延迟是否可接受
3. 检查 `CompID` 是否匹配当前 session
4. 检查目标序号是否太高或太低
5. 处理 resend range 是否满足
6. 更新 `lastReceivedTime`
7. 清零 `testRequest` 计数
8. 最后才调：
   - `fromAdmin`
   - 或 `fromApp`

这说明：

> 应用层回调发生得比很多人想象得更晚。

在应用拿到消息之前，QuickFIX 已经做完：

- 解析
- 协议字典校验
- 会话级校验

---

## 8. 配置文件是怎么变成运行时 session 的

现在回答第二个核心问题。

这部分最适合看成一条“装配流水线”。

### 8.1 第一步：配置文件先变成 `SessionSettings`

入口通常是：

```cpp
FIX::SessionSettings settings(file);
```

在 `SessionSettings.cpp` 里，这会：

1. 打开文件
2. 交给 `operator>>(istream, SessionSettings&)`

### 8.2 `operator>>` 会先借助 `Settings` 解析 section

解析逻辑大意是：

1. 把整个文件读成 `Settings`
2. 取出 `[DEFAULT]` section
3. 取出所有 `[SESSION]` section
4. 对每个 session：
   - 复制该 section 的 Dictionary
   - merge 默认配置
   - 读出 `BeginString`
   - `SenderCompID`
   - `TargetCompID`
   - 可选 `SessionQualifier`
   - 构造 `SessionID`
   - `s.set(sessionID, dict)`

这意味着：

- `SessionSettings` 不是一个简单字符串 blob
- 它最后会变成：
  - 一份默认 `Dictionary`
  - 一个 `map<SessionID, Dictionary>`

### 8.3 `SessionSettings::set(sessionID, dict)` 又做了一层整理

它会：

1. 检查 session 是否重复
2. 把 `BeginString` / `SenderCompID` / `TargetCompID` 写回字典
3. 再 merge 默认值
4. 调 `validate(settings)`
5. 放进 `m_settings`

### 8.4 `SessionSettings::validate(...)` 只做配置层最基础校验

它重点检查：

- `BeginString` 是否支持
- `ConnectionType` 是否是 `initiator` 或 `acceptor`

注意：

- 它不会在这里创建 `Session`
- 也不会在这里加载 `DataDictionary`
- 更不会在这里启动网络连接

所以 `SessionSettings` 的职责更像：

- “把配置文件规范化成一个结构化配置仓库”

### 8.5 第二步：`Acceptor` / `Initiator` 遍历 `SessionSettings`

这一步很多人第一次读时会漏掉。

真正调用 `SessionFactory::create(...)` 的，不是 `SessionSettings` 自己，而是：

- `Acceptor::initialize()`
- `Initiator::initialize()`

它们都会：

1. `m_settings.getSessions()`
2. 遍历所有 `SessionID`
3. 按 `ConnectionType` 过滤：
   - acceptor 只创建 acceptor session
   - initiator 只创建 initiator session
4. 调 `factory.create(sessionID, m_settings.get(sessionID))`

所以配置 -> 运行时 session 的完整链里，这一步一定要记住。

### 8.6 第三步：`SessionFactory::create(...)` 做真正的装配

这一步才是真正把一份 session 配置变成一个可运行 `Session` 的地方。

它主要做：

1. 校验 `ConnectionType`
2. acceptor 禁止 `SessionQualifier`
3. 决定是否启用数据字典
4. 对 FIXT 强制要求 `DefaultApplVerID`
5. 创建 `DataDictionaryProvider`
6. 处理 transport / application dictionaries
7. 解析时间配置：
   - `StartTime`
   - `EndTime`
   - `StartDay`
   - `EndDay`
   - `UseLocalTime`
   - `NonStopSession`
8. 解析 logon / logout 时间窗口
9. initiator 解析 `HeartBtInt`
10. `new Session(...)`
11. 把各种配置项灌到 `Session` 上

这就是最核心的装配动作。

### 8.7 FIX4.x 和 FIXT 的字典装配不同

`SessionFactory` 里这部分很关键。

如果是普通 FIX4.x：

- `processFixDataDictionary(...)`

它会把同一个 dictionary 同时作为：

- transport dictionary
- application dictionary

如果是 FIXT：

- `processFixtDataDictionaries(...)`

它会分开：

- transport dictionary
- application dictionary

因为 FIXT 传输层和应用层版本可以不同。

### 8.8 `createDataDictionary(...)` 还会根据配置复制并调整 dictionary

它会先按路径缓存基础 dictionary，然后复制一份，再按配置调开关：

- `ValidateFieldsOutOfOrder`
- `ValidateFieldsHaveValues`
- `ValidateUserDefinedFields`
- `AllowUnknownMsgFields`
- `PreserveMessageFieldsOrder`

所以：

- 同一路径 XML 并不意味着所有 session 共用完全同一份行为配置
- 每个 session 还能基于副本再做校验行为调整

### 8.9 第四步：`Session` 构造函数真正把对象落地

当 `SessionFactory` 调 `new Session(...)` 后，会发生：

- store 创建
- log 创建
- 状态初始值设好
- session 注册
- `onCreate` 回调

到这里，配置才终于真正落成一个运行中的 `Session` 对象。

---

## 9. `SessionFactory` 到底往 `Session` 里灌了哪些运行时参数

这一步单独列出来会很清楚。

创建完 `Session` 后，`SessionFactory::create(...)` 会继续调用很多 setter，例如：

- `setSenderDefaultApplVerID(...)`
- `setLogonTime(...)`
- `setSendRedundantResendRequests(...)`
- `setCheckCompId(...)`
- `setCheckLatency(...)`
- `setMaxLatency(...)`
- `setLogonTimeout(...)`
- `setLogoutTimeout(...)`
- `setResetOnLogon(...)`
- `setResetOnLogout(...)`
- `setResetOnDisconnect(...)`
- `setRefreshOnLogon(...)`
- `setMillisecondsInTimeStamp(...)`
- `setTimestampPrecision(...)`
- `setPersistMessages(...)`
- `setValidateLengthAndChecksum(...)`
- `setSendNextExpectedMsgSeqNum(...)`
- `setIsNonStopSession(...)`
- `setAllowedRemoteAddresses(...)`

这说明：

- `Session` 构造函数只拿最核心的骨架参数
- 其余大量行为开关是在 factory 创建后继续灌进去的

所以 `SessionFactory` 非常像：

- “运行时 Session 装配器”

---

## 10. 消息校验到底发生在什么阶段

现在回答第三个核心问题。

这个问题最容易被误解成“调用了 `DataDictionary::validate` 就算校验了”。

实际上消息校验是分层发生的。

---

## 11. 第一层校验：字符串解析阶段

入口是：

- `Session::next(const std::string &msg, ...)`

这里会构造：

- `Message(msg, sessionDD, appDD, m_validateLengthAndChecksum)`

这至少意味着：

- 原始字符串必须能被解析成 Message
- 如果开启了 `ValidateLengthAndChecksum`，长度和校验和会被检查

这一层更像：

- “消息能不能被成功解码”

如果连这里都过不了，根本进不到后面的 session 验证和应用回调。

---

## 12. 第二层校验：`DataDictionary::validate(...)`

这一步发生在：

- `Session::next(const Message&, ...)`

里，而且发生得很早。

### 12.1 调用时机

在 `Session::next(const Message&, ...)` 中，顺序是：

1. 先检查 session time
2. 先确保 header 基本字段存在
3. 检查 BeginString
4. 设置 FIXT 目标默认应用版本
5. 再取 dictionary
6. 调 `DataDictionary::validate(...)`

也就是说：

- 应用层还没看到消息
- `verify(...)` 也还没走
- 先进行协议字典校验

### 12.2 `DataDictionary::validate(...)` 做了什么

它的逻辑顺序大致是：

1. 检查版本
   如果 session dictionary 自带版本信息，BeginString 不匹配就 `UnsupportedVersion`

2. 检查结构顺序
   如果启用字段顺序校验，就看 `message.hasValidStructure(...)`

3. 检查消息类型和 required 字段
   - `checkMsgType(msgType)`
   - `checkHasRequired(header, body, trailer, msgType)`

4. 迭代 Header 和 Trailer

5. 迭代 Body

这一步很重要，因为它说明：

- `validate()` 不是只检查一件事
- 它是一整套协议级检查总入口

### 12.3 `iterate(...)` 里会做哪些细查

在 `iterate(...)` 里，每个字段会经历这些检查：

1. 重复 tag 检查
   `RepeatedTag`

2. 是否有值
   `NoTagValue`

3. 类型格式检查
   `checkValidFormat(...)`

4. 枚举值检查
   `checkValue(...)`

5. tag number 是否存在于规范里
   `checkValidTagNumber(...)`

6. 这个字段是否允许出现在该消息类型里
   `checkIsInMessage(...)`

7. repeating group 计数是否匹配
   `checkGroupCount(...)`

这就是协议层真正干活的地方。

### 12.4 `checkHasRequired(...)` 很值得单独记住

它会分别检查：

1. Header 必填字段
2. Trailer 必填字段
3. 当前 `MsgType` 下 Body 必填字段
4. 每个 repeating group 内部的必填字段

这意味着：

- required 检查不是只看 body
- header/trailer/group 都会被递归检查

### 12.5 FIXT 场景下为什么要分 sessionDD 和 applicationDD

在 FIXT 模式里：

- transport 层用 session dictionary
- app message 用 application dictionary

所以：

- header/trailer 的 session 语义
- body 的 app 语义

可以分别校验。

这一点是 `DataDictionaryProvider` 存在的重要原因。

---

## 13. 第三层校验：`Session::verify(...)`

这一步和 `DataDictionary::validate()` 完全不是一回事。

### 13.1 `DataDictionary` 校验的是“协议定义”

例如：

- 这个 tag 合法吗
- 这个值格式对吗
- 这个字段该不该在这条消息里

### 13.2 `verify()` 校验的是“会话语义”

例如：

- 当前 logon 状态允不允许出现这个消息
- SendingTime 延迟是否太大
- CompID 对不对
- 消息序号高了还是低了

所以你可以这样区分：

- `DataDictionary`
  在问“这条消息像不像一条合法 FIX 消息”

- `Session::verify`
  在问“这条消息在当前这个会话上下文里能不能接受”

### 13.3 `verify()` 的顺序也很关键

它会：

1. 取 Header 里的：
   - `MsgType`
   - `SenderCompID`
   - `TargetCompID`
   - `SendingTime`
   - 需要时再取 `MsgSeqNum`

2. `validLogonState(...)`

3. `isGoodTime(sendingTime)`

4. `isCorrectCompID(senderCompID, targetCompID)`

5. 检查高低序号

6. 必要时更新 resend range 状态

7. 更新 `lastReceivedTime`

8. 清零 test request 计数

9. 最后才走 `fromAdmin` / `fromApp`

这一点尤其重要：

> 应用回调是 verify 成功之后才发生的。

### 13.4 高序号和低序号时会怎样

#### 序号太高

`doTargetTooHigh(msg)` 会：

1. 记录事件
2. 把消息先 queue 起来
3. 如果必要，发 `ResendRequest`

这说明：

- 高序号消息通常不会立即交给应用层
- 会先等缺口补齐

#### 序号太低

`doTargetTooLow(msg)` 会：

1. 看是不是 `PossDup`
2. 不是的话，一般发 Logout 并报错
3. 是的话，再走重复消息处理逻辑

这说明：

- 低序号消息通常被视作严重会话问题

### 13.5 为什么 verify 结束后还会改状态

因为 verify 成功后，QuickFIX 不只是“接受这条消息”，还会更新会话时钟：

- `lastReceivedTime = now`
- `testRequest = 0`

这会影响后面的心跳和超时逻辑。

所以 verify 也是状态推进的一部分。

---

## 14. admin 消息和 app 消息在会话层的分流

在 `Session::next(const Message&, ...)` 里，`MsgType` 会先做一层大分流。

### 14.1 admin 消息的专门路径

这些消息会走专门处理函数：

- Logon
- Heartbeat
- TestRequest
- SequenceReset
- Logout
- ResendRequest
- Reject

也就是说会话层自己吃掉了这些协议行为。

### 14.2 app 消息的路径

普通应用消息则会：

1. `verify(...)`
2. 成功后 `m_state.incrNextTargetMsgSeqNum()`
3. 最终由 `verify()` 内部触发：
   - `m_application.fromApp(...)`

这意味着：

- app 消息的业务处理入口，还是你在第一阶段看到的 `fromApp`
- 但它到达那里之前，已经先经过第三阶段这整套引擎了

---

## 15. 各种异常在会话层是怎么被转换成 Reject 的

这一部分很值得注意，因为它非常体现 QuickFIX 的工程风格。

`Session::next(const Message&, ...)` 对很多异常都有显式 catch：

- `RequiredTagMissing`
- `FieldNotFound`
- `InvalidTagNumber`
- `NoTagValue`
- `TagNotDefinedForMessage`
- `InvalidMessageType`
- `UnsupportedMessageType`
- `TagOutOfOrder`
- `IncorrectDataFormat`
- `IncorrectTagValue`
- `RepeatedTag`
- `RepeatingGroupCountMismatch`
- `RejectLogon`
- `UnsupportedVersion`

然后分别转成：

- `generateReject(...)`
- `generateBusinessReject(...)`
- `generateLogout(...)`
- `disconnect()`

这说明 QuickFIX 会话层的处理思路是：

- 底层异常不是直接炸出程序
- 而是尽量翻译成符合 FIX 语义的回包或断开动作

---

## 16. 现在把“配置 -> 会话 -> 校验 -> 应用”串成一条完整链

这条链你最好能自己顺口讲出来。

### 16.1 配置装配链

```text
.cfg 文件
-> SessionSettings(file)
-> 解析 [DEFAULT] / [SESSION]
-> 生成 SessionID -> Dictionary 映射
-> Acceptor/Initiator.initialize()
-> 遍历 settings.getSessions()
-> SessionFactory::create(sessionID, dict)
-> 创建 DataDictionaryProvider / TimeRange / HeartBtInt
-> new Session(...)
-> 注册 session，创建 store/log，触发 onCreate
```

### 16.2 入站消息链

```text
网络字符串
-> Session::next(rawString, now)
-> Message(rawString, dictionaries, validateLengthChecksum)
-> Session::next(message, now)
-> DataDictionary::validate(...)
-> admin 分派 或 verify(...)
-> fromAdmin / fromApp
```

### 16.3 出站消息链

```text
应用构造 Message
-> Session::send(message)
-> sendRaw(message)
-> fill(header)
-> toAdmin / toApp
-> toString()
-> persist()
-> responder.send()
```

只要这三条链你能讲顺，第三阶段就已经抓到主干了。

---

## 17. 这阶段最容易混淆的三个点

### 17.1 `SessionSettings` 不会创建 `Session`

它只是：

- 读取配置
- 组织默认值和每个 session 的字典

真正创建 session 的是：

- `Acceptor/Initiator + SessionFactory`

### 17.2 `DataDictionary::validate` 不是全部校验

它负责：

- 协议定义和字段语义校验

但不负责：

- CompID 是否匹配当前会话
- 序号是否合理
- 当前是否允许发这种消息
- 心跳/超时语义

这些是 `Session::verify()` 的事。

### 17.3 应用回调不是第一时间发生的

收到消息后并不是马上：

- `fromApp(message, sessionID)`

而是先经历：

1. 解析
2. 字典校验
3. 会话校验

最后才进入应用层。

---

## 18. 现在回头再看这些文件，建议怎么读

这一阶段最适合按下面顺序读。

### 第一步：先看配置装配线

按这个顺序：

1. `SessionSettings.h`
2. `SessionSettings.cpp`
3. `Acceptor.cpp` 或 `Initiator.cpp` 的 `initialize()`
4. `SessionFactory.h`
5. `SessionFactory.cpp`

你的目标只回答一个问题：

> 一份配置是怎样变成一个 `Session` 对象的？

### 第二步：再看 `SessionState.h`

只盯这些成员：

- `receivedLogon`
- `sentLogon`
- `sentLogout`
- `sentReset`
- `receivedReset`
- `heartBtInt`
- `lastSentTime`
- `lastReceivedTime`
- `resendRange`
- `queue`

你的目标是：

> 先知道这台状态机手里到底握着哪些状态变量。

### 第三步：再看 `Session.cpp` 的四个总入口

优先读：

1. `next(const UtcTimeStamp &now)`
2. `next(const std::string &msg, ...)`
3. `next(const Message &message, ...)`
4. `sendRaw(...)`

你的目标是：

> 先把“定时推进、入站字符串、入站消息、出站消息”四条总路径分清楚。

### 第四步：再看 admin 消息专用函数

重点读：

1. `nextLogon(...)`
2. `generateLogon()`
3. `nextResendRequest(...)`
4. `generateResendRequest(...)`
5. `nextSequenceReset(...)`
6. `disconnect()`

你的目标是：

> 看懂登录、重传、断开这些关键管理动作是怎么推进状态的。

### 第五步：最后读 `DataDictionary.cpp`

重点看：

1. `validate(...)`
2. `iterate(...)`
3. `checkHasRequired(...)`
4. `checkValidFormat(...)`
5. `checkValue(...)`
6. `checkIsInMessage(...)`
7. `checkGroupCount(...)`

你的目标是：

> 把协议校验和会话校验彻底区分开。

---

## 19. 第三阶段结束后，你应该能回答什么

如果这一阶段已经吃透，你现在应该能比较顺地回答这些问题：

1. `Session` 和 `SessionState` 各自负责什么？
2. `next(now)`、`next(rawString, now)`、`next(message, now)` 三个入口分别做什么？
3. initiator 和 acceptor 的 session 初始状态有什么区别？
4. `HeartBtInt` 为什么会影响 initiator / acceptor 身份判断？
5. 为什么 `Acceptor/Initiator` 才是把 `SessionSettings` 变成运行时 `Session` 的外层入口？
6. `SessionFactory` 在创建 session 时到底灌进了哪些行为参数？
7. `DataDictionary::validate()` 和 `Session::verify()` 各校验什么？
8. app 消息为什么不会在刚收到时就立刻交给 `fromApp()`？
9. 高序号消息为什么会先 queue 起来？
10. 为什么 QuickFIX 能把很多底层异常转成 FIX Reject / BusinessReject / Logout？

如果这些问题你已经基本能自己讲出来，第三阶段就算真正过关了。

---

## 20. 这一阶段压缩成一句话

最后把第三阶段压缩成一句最关键的话：

> `SessionSettings` 先把配置文件组织成 `SessionID -> Dictionary` 的结构；`Acceptor/Initiator` 再遍历这些配置交给 `SessionFactory` 组装成真正的 `Session` 对象；运行时 `Session` 通过 `SessionState` 维护登录、序号、心跳、超时和重传状态，并在原始字符串解析之后先做 `DataDictionary` 协议校验，再做 `Session::verify` 会话校验，最后才把合法消息交给应用层。

---

## 21. 下一步最自然的衔接

第三阶段之后，最自然的下一步通常有两条路：

1. 继续往下走网络与 I/O 层
   例如：
   - `SocketInitiator.cpp`
   - `SocketAcceptor.cpp`
   - `SocketConnection.cpp`
   - `FileStore.cpp`
   - `FileLog.cpp`

2. 或者回过头做一条完整运行链路追踪
   例如：
   - 从 `tradeclient` 发一条 `NewOrderSingle`
   - 进入 `Session::sendRaw`
   - 对端 `Session::next(rawString)`
   - `DataDictionary::validate`
   - `verify`
   - `fromApp`
   - `crack`
   - `onMessage`

对你当前这套学习路线来说，第四阶段如果继续写成文档，通常最适合就接“网络与持久化层”。

