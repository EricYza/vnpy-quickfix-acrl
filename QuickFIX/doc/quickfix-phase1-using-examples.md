# QuickFIX 第一阶段导读：先把“怎么用”看懂

这份文档只做第一阶段的事情：

1. 看懂一个 QuickFIX 应用是怎么启动起来的。
2. 看懂 `Application` 回调为什么会被调用。
3. 看懂 `crack()` 为什么能把通用消息分发到具体 `onMessage(...)`。
4. 把 `tradeclient`、`executor`、`ordermatch` 三个示例程序串成一张完整的使用脑图。

这份文档故意不深入第二阶段以后那些更底层的实现，例如：

- `Session.cpp` 里序号和心跳到底怎么推进
- `Message.cpp` 里字符串到底怎么解析
- `DataDictionary` 里校验规则怎么实现

那些会放到后续阶段。第一阶段的目标很明确：

> 先会“顺着示例程序读懂 QuickFIX 是怎么被用起来的”。

---

## 1. 第一阶段我们到底在读什么

你之前的阅读顺序里有四个文件：

- `examples/tradeclient/tradeclient.cpp`
- `examples/tradeclient/Application.cpp`
- `examples/executor/C++/executor.cpp`
- `examples/executor/C++/Application.cpp`

这四个文件确实是第一阶段最核心的起点。

但如果只看它们，会漏掉一个很重要的示例：

- `examples/ordermatch/ordermatch.cpp`
- `examples/ordermatch/Application.cpp`

所以这一阶段我建议你把第一阶段的“应用层源码组”理解成三套示例：

1. `tradeclient`
   主动发起连接的客户端示例

2. `executor`
   被动接受连接的最小服务端示例

3. `ordermatch`
   被动接受连接的更完整撮合示例

它们共同回答的是：

- QuickFIX 库怎样被组装起来
- 你的 `Application` 类怎样接到 QuickFIX 引擎上
- 引擎收到消息后，怎样进入你的业务函数

---

## 2. 先建立一个最重要的总图

不管是 `tradeclient`、`executor` 还是 `ordermatch`，它们的共通结构都差不多：

1. 读配置文件
2. 创建你自己的 `Application` 对象
3. 创建消息存储工厂和日志工厂
4. 创建 Initiator 或 Acceptor
5. `start()` 启动 QuickFIX 引擎
6. 引擎在后台处理连接、登录、收发消息
7. 遇到事件时回调你的 `Application`
8. 你在 `Application` 里写自己的应用逻辑

把它压缩成一句话：

> 示例程序做的事情，不是“自己实现 FIX 引擎”，而是“把 QuickFIX 引擎实例化，然后把自己的应用逻辑挂进去”。

---

## 3. 三个示例程序各自扮演什么角色

在进具体代码之前，先分清角色。

| 示例 | 网络角色 | 你从它学什么 |
|---|---|---|
| `tradeclient` | Initiator，主动连对方 | 怎么构造消息并发送 |
| `executor` | Acceptor，等待别人连我 | 怎么收消息并回复最简单的业务结果 |
| `ordermatch` | Acceptor，等待别人连我 | 怎么在应用层做更完整的订单处理 |

你可以把它们理解成：

- `tradeclient` 是“前台下单终端”
- `executor` 是“最小版柜台”
- `ordermatch` 是“多做了一点业务逻辑的柜台”

它们底层都在用同一个 QuickFIX 核心库。

---

## 4. 先看最外层：一个示例程序是怎么启动 QuickFIX 的

先把最外层骨架看懂，这样你读三个示例时就不会迷路。

### 4.1 `tradeclient.cpp` 的启动骨架

`examples/tradeclient/tradeclient.cpp` 里最关键的逻辑可以概括成这样：

```cpp
FIX::SessionSettings settings(file);

Application application;
FIX::FileStoreFactory storeFactory(settings);
FIX::ScreenLogFactory logFactory(settings);

std::unique_ptr<FIX::Initiator> initiator =
    std::make_unique<FIX::SocketInitiator>(application, storeFactory, settings, logFactory);

initiator->start();
application.run();
initiator->stop();
```

这里面每一行都很重要。

### 4.2 `executor.cpp` 的启动骨架

`examples/executor/C++/executor.cpp` 则是对应的服务端版本：

```cpp
FIX::SessionSettings settings(file);

Application application;
FIX::FileStoreFactory storeFactory(settings);
FIX::ScreenLogFactory logFactory(settings);

std::unique_ptr<FIX::Acceptor> acceptor =
    std::make_unique<FIX::SocketAcceptor>(application, storeFactory, settings, logFactory);

acceptor->start();
wait();
acceptor->stop();
```

它和 `tradeclient` 的区别非常集中：

- `tradeclient` 创建的是 `Initiator`
- `executor` 创建的是 `Acceptor`

也就是：

- `Initiator` 负责“主动连出去”
- `Acceptor` 负责“被动监听端口”

### 4.3 `ordermatch.cpp` 的启动骨架

`examples/ordermatch/ordermatch.cpp` 也是 Acceptor，但比 `executor` 多了一层本地交互：

```cpp
FIX::SessionSettings settings(file);

Application application;
FIX::FileStoreFactory storeFactory(settings);
FIX::ScreenLogFactory logFactory(settings);
FIX::SocketAcceptor acceptor(application, storeFactory, settings, logFactory);

acceptor.start();
while (true) {
  std::cin >> value;
  ...
}
acceptor.stop();
```

这说明：

- `ordermatch` 不是纯后台程序
- 它除了收 FIX 消息之外，还允许你在本地命令行查看订单簿

例如：

- `#symbols`
- `#quit`
- 或输入某个 symbol 来查看该标的订单簿

### 4.4 三个示例的共同外壳

它们虽然业务不同，但外壳几乎一致：

1. `SessionSettings`
   读取 `.cfg` 配置文件

2. `Application`
   你的应用层类，所有回调都写在这里

3. `FileStoreFactory`
   消息存储工厂，决定会话状态和消息落盘方式

4. `ScreenLogFactory`
   日志工厂，决定日志怎么打印

5. `SocketInitiator` / `SocketAcceptor`
   真正把 Application 接到网络和会话引擎上

也就是说：

> 示例程序的 `main()` 函数，本质上是在做“依赖注入”和“装配”。

---

## 5. 把这几个对象讲通俗一点

### 5.1 `SessionSettings`

它的作用是读配置文件，把配置变成运行时可用的 session 设置。

里面包括：

- BeginString
- SenderCompID
- TargetCompID
- SocketConnectHost / SocketConnectPort
- HeartBtInt
- FileStorePath
- FileLogPath
- SSL 参数

你可以把它看成：

- “把 `.cfg` 文件变成 C++ 里的配置对象”

### 5.2 `Application`

这是你最重要的“应用逻辑入口”。

QuickFIX 不知道你要做撮合、风控、回报还是行情，它只知道：

- 某个 session 建好了
- 某条管理消息要发
- 某条应用消息收到了

于是它把这些事件通过回调交给 `Application`。

### 5.3 `FileStoreFactory`

QuickFIX 的 session 有状态。

例如：

- 下一个发送序号是多少
- 收到了哪些消息
- 某些消息要不要持久化

`FileStoreFactory` 的意思就是：

- “请用文件系统来做这些存储”

### 5.4 `ScreenLogFactory`

这个就是：

- “请把日志打印到屏幕上”

所以你运行示例时看到的那些：

- `<20260706-09:30:46..., incoming>`
- `(Received logon request)`

就是这一层在起作用。

### 5.5 `SocketInitiator` / `SocketAcceptor`

这两个类是把“会话引擎”和“网络 socket”接起来的桥梁。

可以简单记成：

- `Initiator` 主动连对方
- `Acceptor` 等别人连我

它们创建时都需要拿到：

- `Application`
- `StoreFactory`
- `SessionSettings`
- `LogFactory`

这很重要，因为这说明：

- QuickFIX 引擎不是一个“全局单例程序”
- 它是被你的应用实例化出来的

---

## 6. `Application` 回调为什么会被调用

这是第一阶段的核心问题之一。

### 6.1 QuickFIX 要求你的应用实现一个接口

公共接口在：

- `include/quickfix/Application.h`

里面定义了这些纯虚函数：

- `onCreate`
- `onLogon`
- `onLogout`
- `toAdmin`
- `toApp`
- `fromAdmin`
- `fromApp`

也就是说，QuickFIX 的思路是：

> 引擎负责底层协议和网络；应用负责响应这些事件。

### 6.2 为什么 `Application application;` 这一句就能“接上回调”

因为后面你把这个对象传给了 Initiator / Acceptor：

```cpp
FIX::SocketInitiator(application, storeFactory, settings, logFactory)
```

或：

```cpp
FIX::SocketAcceptor(application, storeFactory, settings, logFactory)
```

从这一刻开始，QuickFIX 内部就持有了这个 `Application` 的引用，并在适当时机调用它的方法。

所以这里真正发生的是：

1. 你的 `Application` 对象被创建
2. 它作为接口实现，被交给 QuickFIX 引擎
3. 引擎在 session 生命周期和消息收发过程中回调它

### 6.3 这几个回调分别表示什么

这是第一阶段最需要建立的感觉。

#### `onCreate(const SessionID&)`

会话对象创建时调用。

注意：

- 这不等于已经连上了
- 也不等于已经登录成功

它更像：

- “这个 session 在内存里建立起来了”

#### `onLogon(const SessionID&)`

登录成功后调用。

这是你在示例里看到：

```text
Logon - FIX.4.2:CLIENT1->EXECUTOR
```

的来源。

#### `onLogout(const SessionID&)`

登出或断开时调用。

#### `toAdmin(Message&, const SessionID&)`

有 admin 消息要发出去之前调用。

典型 admin 消息包括：

- Logon
- Logout
- Heartbeat
- TestRequest
- Reject

这一层常用于：

- 登录消息里补用户名密码
- 修改管理消息字段

#### `fromAdmin(const Message&, const SessionID&)`

收到 admin 消息时调用。

通常可用于：

- 验证对方登录信息
- 审核管理消息

#### `toApp(Message&, const SessionID&)`

有应用消息要发给对方时调用。

比如：

- NewOrderSingle
- OrderCancelRequest
- ExecutionReport

#### `fromApp(const Message&, const SessionID&)`

收到应用消息时调用。

这个最关键，因为大多数“业务消息处理”都是从这里开始的。

---

## 7. 先看 `tradeclient`：它到底怎么“用” QuickFIX

`tradeclient` 是学习 QuickFIX 最好的第一站，因为它最直观。

你会看到：

- 它自己不处理网络细节
- 它主要做菜单输入、构造消息、发送消息、打印消息

### 7.1 `tradeclient.cpp` 只负责“把程序跑起来”

这个文件的职责非常单纯：

1. 检查命令行参数
2. 读取配置文件路径
3. 创建 `SessionSettings`
4. 创建 `Application`
5. 创建 `FileStoreFactory`
6. 创建 `ScreenLogFactory`
7. 选择 Initiator 类型
8. `initiator->start()`
9. 调 `application.run()`
10. 退出时 `initiator->stop()`

这说明：

- `tradeclient.cpp` 不是业务逻辑核心
- 它是启动器

### 7.2 `tradeclient` 为什么有 SSL 分支

在 `tradeclient.cpp` 里你会看到：

- `SocketInitiator`
- `ThreadedSSLSocketInitiator`
- `SSLSocketInitiator`

它的含义很朴素：

- 如果命令行没带 `SSL`，走普通 TCP
- 如果带了 `SSL` 或 `SSL-ST`，走 SSL 版本的 Initiator

你之前运行：

```bash
./tradeclient cfg/tradeclient_ssl_local.cfg SSL
```

就是在选择 SSL 版本。

### 7.3 `Application.h` 才是关键：它同时继承了两个基类

`examples/tradeclient/Application.h` 里最重要的一行是：

```cpp
class Application : public FIX::Application, public FIX::MessageCracker
```

这是第一阶段必须吃透的一行。

它表示这个类同时承担两件事：

1. 它是一个 QuickFIX `Application`
   所以它能接收引擎回调

2. 它是一个 `MessageCracker`
   所以它具备“把通用消息分发成具体消息类型”的能力

这就是为什么后面你会看到：

- `fromApp(...)`
- `crack(message, sessionID)`
- `onMessage(const FIX42::ExecutionReport&, ...)`

它们能够连成一条链。

### 7.4 `tradeclient::run()` 是程序自己的菜单主循环

`Application.cpp` 里的 `run()` 其实不属于 QuickFIX 回调体系，它是示例程序自己写的交互主循环。

它做的是：

1. 打印菜单
2. 让你选动作
3. 根据动作调用：
   - `queryEnterOrder()`
   - `queryCancelOrder()`
   - `queryReplaceOrder()`
   - `queryMarketDataRequest()`

所以要区分两种“流程”：

1. QuickFIX 回调流程
   由引擎触发

2. 应用交互流程
   由你在终端输入触发

`tradeclient` 两者都有。

### 7.5 `queryEnterOrder()` 做了什么

以发单为例：

1. 先选 FIX 版本
2. 根据版本调用对应的构造函数
   - `queryNewOrderSingle40()`
   - `queryNewOrderSingle41()`
   - `queryNewOrderSingle42()`
   - ...
3. 每个函数返回一个强类型的 `NewOrderSingle`
4. 再通过：

```cpp
FIX::Session::sendToTarget(order);
```

发出去

这一步非常关键，因为它告诉你：

> 发送应用消息，不是直接用 socket 写字符串，而是构造一个 `FIX::Message` 对象，然后交给 `Session`。

### 7.6 `queryNewOrderSingle42()` 很值得精读

例如 FIX42 版本：

```cpp
FIX42::NewOrderSingle newOrderSingle(
    queryClOrdID(),
    FIX::HandlInst('1'),
    querySymbol(),
    querySide(),
    FIX::TransactTime(),
    ordType = queryOrdType());
```

接着再补：

- `OrderQty`
- `TimeInForce`
- 可能的 `Price`
- 可能的 `StopPx`
- 最后 `queryHeader(...)`

这里你可以学到两个非常重要的使用方式：

1. QuickFIX 提供了强类型消息类
   不需要你手写 `35=D|11=...|55=...`

2. Header 也是单独设置的
   所以 `SenderCompID` / `TargetCompID` 并不是“构造函数里自动带上的”

### 7.7 `queryHeader()` 说明了 Header 是怎么补的

这个函数做的事情很直白：

```cpp
header.setField(querySenderCompID());
header.setField(queryTargetCompID());
```

必要时还会设置：

- `TargetSubID`

这一步也解释了你之前实际测试时为什么要输入：

- `SenderCompID`
- `TargetCompID`

因为示例程序是在这里手动把它们写入消息头的。

### 7.8 `toApp()` 是发送前的“最后一道钩子”

`tradeclient` 的 `toApp()` 里做了两件事：

1. 检查 `PossDupFlag`
2. 打印 `OUT: ...`

如果 `PossDupFlag` 为真，它会抛 `DoNotSend`。

这说明：

- `toApp()` 不是“消息发出之后”的通知
- 而是“消息将要发出时”的拦截点

### 7.9 `fromApp()` 是收到应用消息后的入口

这里是第一阶段最重要的一行：

```cpp
crack(message, sessionID);
```

然后才打印：

```cpp
std::cout << "IN: " << message << std::endl;
```

这说明 `tradeclient` 的处理思路是：

1. 收到通用 `FIX::Message`
2. 交给 `MessageCracker` 分发
3. 然后打印原始消息

### 7.10 为什么 `tradeclient` 的 `onMessage(...)` 几乎都是空的

你会看到它实现了很多：

- `onMessage(const FIX42::ExecutionReport&, ...)`
- `onMessage(const FIX42::OrderCancelReject&, ...)`
- 其他版本对应的重载

但函数体是空的。

这不是没用，而是示例在表达一种“接口占位”：

- 这些消息类型是 tradeclient 预期可能收到的
- 所以先提供重载，让 `crack()` 能顺利落地
- 但示例没有做更复杂的客户端后处理

也就是说它更像“演示怎么收”，不是“演示完整客户端业务逻辑”。

---

## 8. `executor`：最小服务端是怎么接消息并回复的

如果说 `tradeclient` 让你看懂“怎么发”，那么 `executor` 让你看懂“怎么收并回”。

### 8.1 `executor.cpp` 和 `tradeclient.cpp` 的结构几乎镜像

对应关系非常清楚：

- `tradeclient` 用 `Initiator`
- `executor` 用 `Acceptor`

其余启动部件几乎一样：

- `SessionSettings`
- `Application`
- `FileStoreFactory`
- `ScreenLogFactory`

这说明 QuickFIX 应用的标准装配方式是非常统一的。

### 8.2 `wait()` 暗示了 executor 是纯后台服务

`executor.cpp` 在 `acceptor->start()` 后进入：

```cpp
while (true) {
  FIX::process_sleep(1);
}
```

这说明：

- 它不像 `tradeclient` 那样主动给用户菜单
- 它只是启动后一直等别人发消息过来

### 8.3 `executor` 的 `Application.h` 很能说明“业务边界”

它只声明了这些消息处理重载：

- `onMessage(const FIX40::NewOrderSingle&, ...)`
- `onMessage(const FIX41::NewOrderSingle&, ...)`
- `onMessage(const FIX42::NewOrderSingle&, ...)`
- `onMessage(const FIX43::NewOrderSingle&, ...)`
- `onMessage(const FIX44::NewOrderSingle&, ...)`
- `onMessage(const FIX50::NewOrderSingle&, ...)`

这几乎等于直接告诉你：

> 这个示例服务端只打算处理 NewOrderSingle。

所以当你之前测试：

- Cancel Replace
- 其他业务消息

返回 `Unsupported Message Type` 时，根源就在这里。

不是 QuickFIX 核心库完全不会解析 `35=G`，而是：

- `executor` 这个示例应用没有为它提供对应的 `onMessage(...)`

### 8.4 `fromApp()` 仍然是入口，但这次会真的落到业务逻辑

和 `tradeclient` 一样，`executor` 收到应用消息后也做：

```cpp
crack(message, sessionID);
```

但这次，因为它实现了 `NewOrderSingle` 的重载，所以 `crack()` 会真的落到业务处理函数里。

### 8.5 以 `FIX42::NewOrderSingle` 为例，看它是怎么处理订单的

大致流程是：

1. 从消息里取出：
   - `Symbol`
   - `Side`
   - `OrdType`
   - `OrderQty`
   - `Price`
   - `ClOrdID`
   - `Account`

2. 检查：

```cpp
if (ordType != FIX::OrdType_LIMIT) {
  throw FIX::IncorrectTagValue(ordType.getTag());
}
```

这就是你之前 market 单被拒绝的直接原因。

3. 构造 `ExecutionReport`
4. 用：

```cpp
FIX::Session::sendToTarget(executionReport, sessionID);
```

发回去

所以 `executor` 的业务本质是：

- 收到限价单
- 直接视为成交
- 回一个成交回报

它不是完整交易系统，只是一个最小可运行示例。

### 8.6 `sendToTarget(..., sessionID)` 很关键

客户端发消息时，常见写法是：

```cpp
FIX::Session::sendToTarget(order);
```

服务端回复时，常见写法是：

```cpp
FIX::Session::sendToTarget(executionReport, sessionID);
```

这里的区别是：

- 客户端示例通常已经知道要发往哪个 session
- 服务端收到消息后，需要按当前收到的这个 session 原路回去

也就是：

- `sessionID` 充当“回给谁”的定位信息

### 8.7 `executor` 为什么看起来“懂很多 FIX 版本”

因为它写了多套重载：

- FIX40
- FIX41
- FIX42
- FIX43
- FIX44
- FIX50

但你会发现这些函数结构非常像。

这说明它不是写了 6 套完全不同的业务逻辑，而是在演示：

- QuickFIX 可以对不同 BeginString 的消息做版本化强类型处理

也就是说：

> 示例在强调的是“版本适配能力”，不是“复杂业务能力”。

---

## 9. `ordermatch`：为什么它也应该纳入第一阶段

如果只读 `tradeclient + executor`，你会得到一个印象：

- QuickFIX 示例程序只是收一个单、回一个单

这会有点误导。

因为 `ordermatch` 才更像“稍微有点业务味道”的例子，它让你看到：

- 同样的回调和 `crack()` 机制
- 可以挂上更复杂的应用处理

### 9.1 `ordermatch.cpp` 的外壳和 `executor` 很接近

它也做了：

- 读取 `SessionSettings`
- 创建 `Application`
- 创建 `FileStoreFactory`
- 创建 `ScreenLogFactory`
- 创建 `SocketAcceptor`
- `acceptor.start()`

所以网络和会话装配层没有本质区别。

### 9.2 `ordermatch` 的不同点在于“它有自己的内存订单簿”

它的 `Application.h` 里有：

- `OrderMatcher m_orderMatcher`
- `IDGenerator m_generator`

这就说明：

- 它不是收到消息就立刻简单回复
- 它会在应用层维护订单状态

### 9.3 `ordermatch` 支持哪些消息

在 `Application.h` 里你会看到它实现了：

- `onMessage(const FIX42::NewOrderSingle&, ...)`
- `onMessage(const FIX42::OrderCancelRequest&, ...)`
- `onMessage(const FIX42::MarketDataRequest&, ...)`
- `onMessage(const FIX43::MarketDataRequest&, ...)`

这说明它的目标是演示：

1. 下单
2. 撤单
3. 行情请求

而不是只演示最小的发单回报。

### 9.4 `ordermatch` 的 `fromApp()` 和前两个示例完全同型

它也写：

```cpp
crack(message, sessionID);
```

这件事很重要，因为它告诉你：

> 三个示例的“消息进入业务层”的入口完全一致，区别只在各自实现了哪些 `onMessage(...)`。

### 9.5 `ordermatch` 的 `NewOrderSingle` 处理比 `executor` 更像真实业务

它会：

1. 从 Header 里取：
   - `SenderCompID`
   - `TargetCompID`

2. 从消息体里取：
   - `ClOrdID`
   - `Symbol`
   - `Side`
   - `OrdType`
   - `Price`
   - `OrderQty`
   - `TimeInForce`

3. 做业务限制检查
   - 只支持 Day
   - 只支持 Limit
   - 只支持 buy / sell

4. 构造内部 `Order` 对象
5. 调 `processOrder(order)`

也就是说：

- `executor` 里 onMessage 更像“协议演示”
- `ordermatch` 里 onMessage 更像“业务入口”

### 9.6 `processOrder()` 让你看到应用层该怎么继续写

这个函数大概做：

1. 插入订单簿
2. 如果插入成功，先回 `NEW`
3. 然后撮合
4. 有成交的话，回 `FILLED` 或 `PARTIALLY_FILLED`
5. 插入失败则回 `REJECTED`

这里最值得你建立的感觉是：

> QuickFIX 只负责把 FIX 消息送到你手上；收到之后怎样变成你的内部业务对象，是应用层自己的事。

### 9.7 `processCancel()` 让你看到撤单不是引擎自动做的

撤单处理流程是：

1. 根据 `OrigClOrdID`、`Symbol`、`Side` 找订单
2. 调内部 `order.cancel()`
3. 回一个 canceled 状态
4. 从订单簿中删除

这正说明：

- QuickFIX 不会替你“自动维护订单簿”
- 引擎只负责把 `OrderCancelRequest` 作为消息送进来
- 真正的业务状态变更，是 `Application` 自己写的

### 9.8 `MarketDataRequest` 让你看见 repeating group 的使用痕迹

`ordermatch` 的 `FIX42::MarketDataRequest` 处理里有：

- `NoRelatedSym`
- `getGroup(i, noRelatedSymGroup)`

这会让你第一次接触到：

- FIX 重复组在 QuickFIX 对象模型里怎么被读取

这一点后面第二阶段再深入，但第一阶段至少先知道：

- QuickFIX 不只支持简单平铺字段
- 它也支持 group

### 9.9 `ordermatch.cpp` 的本地命令循环也很有价值

它允许你在程序运行时输入：

- `#symbols`
  查看所有 symbol 的订单簿

- 某个具体 symbol
  查看这个标的的订单簿

- `#quit`
  退出

这表明：

- 一个 QuickFIX 应用不一定只是“收到消息马上返回”
- 它完全可以同时拥有自己的本地控制台、内存状态和其他业务接口

---

## 10. `crack()` 和 `onMessage()` 到底是怎么连起来的

这是第一阶段最关键的第三个目标。

### 10.1 第一步：QuickFIX 先把你收到的内容交成通用 `Message`

在 `fromApp(...)` 里，你拿到的是：

```cpp
const FIX::Message& message
```

这说明在进入你的应用逻辑时，引擎已经至少做完了这些事：

1. 从网络上收到一串 FIX 字符串
2. 解析成 `FIX::Message`
3. 通过会话层判断这是一条应用消息
4. 调用你的 `fromApp(...)`

这一步你现在不需要追它内部实现，只需要知道：

- 到你手上时，它已经是“结构化消息对象”了

### 10.2 第二步：`FIX::MessageCracker` 先按版本分派

公共的 `include/quickfix/MessageCracker.h` 做的第一层分派是：

1. 先读 Header 里的 `BeginString`
2. 判断是：
   - FIX.4.0
   - FIX.4.1
   - FIX.4.2
   - FIX.4.3
   - FIX.4.4
   - FIXT.1.1 / FIX5.x
3. 再把消息转交给对应版本的 `MessageCracker`

所以第一层 `crack()` 解决的问题是：

> 这条消息属于哪个 FIX 版本？

### 10.3 第三步：版本专属 `MessageCracker` 再按 `MsgType` 分派

例如 `src/C++/fix42/MessageCracker.h` 里，核心逻辑类似：

```cpp
if( msgTypeValue == "D" )
  return onMessage( (const NewOrderSingle&)message, sessionID );

if( msgTypeValue == "F" )
  return onMessage( (const OrderCancelRequest&)message, sessionID );

if( msgTypeValue == "G" )
  return onMessage( (const OrderCancelReplaceRequest&)message, sessionID );

if( msgTypeValue == "V" )
  return onMessage( (const MarketDataRequest&)message, sessionID );
```

这第二层 `crack()` 解决的问题是：

> 在 FIX4.2 这个版本里，这条消息具体是哪种消息类型？

### 10.4 所以 `crack()` 实际上做了两层路由

你可以把它拆成：

1. 版本路由
   `BeginString -> FIX42::MessageCracker`

2. 消息类型路由
   `35=D -> onMessage(NewOrderSingle)`

这就是为什么你在自己的 `Application` 里只写：

```cpp
crack(message, sessionID);
```

就够了。

### 10.5 为什么没实现对应 `onMessage(...)` 就会报 `UnsupportedMessageType`

各版本 `MessageCracker` 默认很多 `onMessage(...)` 实现都是：

```cpp
{ throw FIX::UnsupportedMessageType(); }
```

所以如果你的 `Application` 没有覆盖某个消息类型的重载，那么：

1. `crack()` 还是会把消息路由到那个默认实现
2. 默认实现直接抛 `UnsupportedMessageType`

这正是你之前在 `executor` 上测试 Replace 时看到的现象。

### 10.6 用你已经实际见过的例子来串一下

当 `tradeclient` 发一条 FIX42 `NewOrderSingle` 给 `executor` 时，`executor` 侧的逻辑链是：

1. QuickFIX 收到消息字符串
2. 调 `executor.Application::fromApp(message, sessionID)`
3. `fromApp()` 调 `crack(message, sessionID)`
4. `FIX::MessageCracker` 看到 `BeginString=FIX.4.2`
5. 路由到 `FIX42::MessageCracker`
6. `FIX42::MessageCracker` 看到 `35=D`
7. 路由到：

```cpp
Application::onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&)
```

8. 你的业务代码开始执行

这就是第一阶段必须建立的最关键运行时脑图。

---

## 11. 三个示例的 `Application` 分工差异

这部分你后面会经常用到。

### 11.1 `tradeclient::Application`

重点是：

- 菜单交互
- 构造消息
- 发消息
- 打印进出消息

它更像“客户端控制台工具”。

### 11.2 `executor::Application`

重点是：

- 接收 NewOrderSingle
- 检查是否是 limit 单
- 立刻构造 ExecutionReport 回回去

它更像“最小版回报服务”。

### 11.3 `ordermatch::Application`

重点是：

- 把 FIX 消息转成内部订单对象
- 维护内存订单簿
- 处理下单、撤单、撮合、行情请求
- 再把处理结果包装回 FIX 报文

它更像“有一点业务状态的示例交易服务”。

---

## 12. 从“怎么用”的角度看，三个示例共同教会你什么

这一阶段其实不是在教你 QuickFIX 的内部实现，而是在教你它的使用模式。

### 12.1 使用 QuickFIX 的标准骨架

任何应用基本都要经历：

1. 写一个继承 `FIX::Application` 的类
2. 通常再继承 `FIX::MessageCracker`
3. 在 `main()` 里读配置
4. 创建 store / log / initiator 或 acceptor
5. `start()`
6. 在 `fromApp()` 里 `crack()`
7. 在具体 `onMessage(...)` 里写业务

### 12.2 QuickFIX 的核心哲学

这个项目的设计思想非常鲜明：

- FIX 引擎层负责协议、会话、网络
- 应用层负责业务

所以它不要求你：

- 自己维护 TCP socket
- 自己拼接 FIX 字符串
- 自己处理 BeginString / BodyLength / CheckSum
- 自己写消息解析器

它只要求你：

- 收到一条强类型消息后，决定业务上怎么处理

### 12.3 `MessageCracker` 是应用层体验的关键

如果没有 `MessageCracker`，你的 `fromApp()` 里就会充满：

```cpp
if (msgType == "D") ...
if (msgType == "F") ...
if (msgType == "G") ...
```

而有了 `MessageCracker`，你只需要提供：

```cpp
void onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&);
void onMessage(const FIX42::OrderCancelRequest&, const FIX::SessionID&);
```

这会让应用代码清晰很多。

---

## 13. 你现在应该怎样实际阅读这几组文件

为了避免一下子信息太多，我建议你按这个节奏读。

### 第一步：先只看 `main()`

依次看：

- `examples/tradeclient/tradeclient.cpp`
- `examples/executor/C++/executor.cpp`
- `examples/ordermatch/ordermatch.cpp`

只回答两个问题：

1. 这里创建了哪些 QuickFIX 对象？
2. 它是 Initiator 还是 Acceptor？

### 第二步：只看 `Application.h`

依次看：

- `examples/tradeclient/Application.h`
- `examples/executor/C++/Application.h`
- `examples/ordermatch/Application.h`

只回答两个问题：

1. 它实现了哪些回调？
2. 它声明了哪些 `onMessage(...)`？

这一步会非常快地告诉你“这个应用到底打算处理哪些业务消息”。

### 第三步：只看 `fromApp()`

你会发现三个示例都差不多：

```cpp
crack(message, sessionID);
```

这里你的任务只有一个：

- 建立“所有业务消息都先经过 fromApp，再经过 crack”的直觉

### 第四步：再看各自最核心的 `onMessage(...)`

优先顺序我建议是：

1. `executor` 的 `onMessage(FIX42::NewOrderSingle, ...)`
2. `ordermatch` 的 `onMessage(FIX42::NewOrderSingle, ...)`
3. `ordermatch` 的 `onMessage(FIX42::OrderCancelRequest, ...)`
4. `tradeclient` 的 `queryNewOrderSingle42()`

这样你会先看懂：

- 服务端怎么收
- 服务端怎么处理
- 客户端怎么发

---

## 14. 第一阶段最重要的几个结论

到这里，你应该把下面这些结论真正记住。

### 14.1 QuickFIX 应用的 `main()` 主要是装配，不是实现协议

`main()` 的作用是：

- 读配置
- 组装对象
- 启动引擎

### 14.2 `Application` 是 QuickFIX 和你的业务代码之间的边界

引擎通过它把事件交给你。

### 14.3 `fromApp()` 是应用消息进入业务层的入口

大多数业务消息处理都是从这里开始的。

### 14.4 `crack()` 做了两层分发

1. 按 FIX 版本分发
2. 按 `MsgType` 分发

### 14.5 `onMessage(...)` 才是你真正写业务逻辑的地方

这里才是：

- 收订单
- 撤单
- 回报
- 撮合

真正发生的地方。

### 14.6 `executor` 和 `ordermatch` 的差别，不在 QuickFIX 启动方式，而在应用逻辑深度

两者都用同一套引擎装配模式。

差别只是：

- `executor` 很薄
- `ordermatch` 更像一个小业务系统

---

## 15. 这一阶段结束后，你已经具备什么能力

如果这份文档你已经基本读顺了，那么你现在应该已经能回答这些问题：

1. 为什么 `Application` 会被 QuickFIX 回调？
2. `Initiator` 和 `Acceptor` 的角色差别是什么？
3. `tradeclient` 为什么能发出一个 `NewOrderSingle`？
4. `executor` 为什么能把它处理成 `ExecutionReport`？
5. `ordermatch` 为什么能支持撤单和简单撮合？
6. `crack()` 为什么能自动路由到具体 `onMessage(...)`？
7. 为什么某些消息会报 `UnsupportedMessageType`？

如果这些问题你已经基本能说出来，第一阶段就算真正完成了。

---

## 16. 下一步最自然的衔接

第一阶段结束后，最自然的下一阶段就是：

- 不再只看“应用怎么用”
- 开始看“消息对象和分发模型本身怎么实现”

也就是你总览文档里第二阶段那部分：

- `Message.h`
- `FieldMap.h`
- 各版本 `MessageCracker.h`
- 版本化消息类
- `spec/FIX42.xml`

但在进第二阶段之前，建议你先自己做一个很短的复盘：

> 从 `tradeclient` 发一笔 FIX42 `NewOrderSingle` 到 `executor` 或 `ordermatch`，把“main -> Application -> fromApp -> crack -> onMessage -> sendToTarget”这条链自己口头讲一遍。

只要这条链讲顺了，后面读底层实现会轻松很多。

