# QuickFIX 链路详解

这份文档把我们前面聊过的几条关键链路集中整理起来，目标是让你能把下面这几件事彻底串起来：

- vn.py 下单后，消息是怎么一路变成 FIX 报文发给 `ordermatch` 的
- `ordermatch` 收到报文后，QuickFIX C++ 内核是怎么解析、校验、分发、撮合的
- `ordermatch` 处理完业务逻辑后，是怎么构造并发回 `ExecutionReport` / `MarketDataSnapshot` 的
- `vnpy_quickfix_gateway` 这边是怎么收到 FIX 回报、解析字段、再推给 vn.py GUI 的
- QuickFIX 当前实现里，重复 `message.get(...)`、`Message::setString()`、`DataDictionary::validate()` 这些性能点分别处在什么位置
- QuickFIX 自带 `tradeclient` 和 `executor` example 的命令行如何使用

本文主要围绕当前这套项目上下文：

- `vnpy_quickfix_gateway/`
- `../vnpy`
- `../QuickFIX`
- 当前重点协议版本：`FIX.4.2`

真正在线路上传输时，FIX 字段分隔符是 `SOH`，也就是 `\x01`。为了阅读方便，下面示例都用 `|` 代替。

---

## 1. 整体分层

先用一句话把整个项目的分工说清楚：

- `vn.py`
  - 产生业务动作，例如下单、撤单、订阅
- `vnpy_quickfix_gateway`
  - 把 vn.py 的业务对象翻译成 QuickFIX 的消息对象
  - 把 QuickFIX 回报再翻译回 vn.py 的 `OrderData` / `TradeData` / `TickData`
- QuickFIX Python 绑定
  - 暴露 Python 可调用的 `fix.Message`、`fix.Session`、`SocketInitiator`、回调接口
- QuickFIX C++ 内核
  - 处理真正的会话管理、消息序列化、反序列化、数据字典校验、socket 收发
- `ordermatch`
  - 作为服务端业务应用，负责接收订单、撮合、返回 `ExecutionReport`

所以你可以把这套系统理解成：

1. vn.py 负责“人机交互”和交易前端语义
2. gateway 负责“业务语义 <-> FIX 语义”的翻译
3. QuickFIX 内核负责“FIX 语义 <-> 网络字节流”的翻译
4. ordermatch 负责“服务端业务处理”

---

## 2. 链路 A：vn.py 发单到 ordermatch

### 2.1 真实示例

我们用这条真实发出去的 `NewOrderSingle` 作为例子：

```text
8=FIX.4.2|9=165|35=D|34=2|49=CLIENT1|52=20260709-02:18:05.105435|56=ORDERMATCH|11=VN20260709021805104180|21=1|38=100|40=2|44=10.5|54=1|55=VNPY021805103315|59=0|60=20260709-02:18:05|10=043|
```

业务含义是：

- 客户端 `CLIENT1`
- 发给服务端 `ORDERMATCH`
- 发一笔限价买单
- 合约 `VNPY021805103315`
- 买入 `100`
- 价格 `10.5`
- 客户订单号 `VN20260709021805104180`

### 2.2 从 GUI 到 gateway

如果是在 vn.py GUI 里点下单，最开始进入的是：

- `vnpy/vnpy/trader/ui/widget.py`
  - `TradingWidget.send_order()`

它会把 GUI 表单内容组装成一个 `OrderRequest`，典型形态类似：

```python
OrderRequest(
    symbol="VNPY021805103315",
    exchange=Exchange.LOCAL,
    direction=Direction.LONG,
    type=OrderType.LIMIT,
    volume=100,
    price=10.5,
    offset=Offset.NONE,
    reference=""
)
```

然后调用：

- `vnpy/vnpy/trader/engine.py`
  - `MainEngine.send_order(req, gateway_name)`

其中 `gateway_name` 就是 `QUICKFIX`。

### 2.3 从 gateway 到 QuickFIX Python

进入：

- `vnpy_quickfix_gateway/vnpy_quickfix_gateway/gateway.py`
  - `QuickfixGateway.send_order()`

这里做了 3 件重要的事：

1. 生成本地订单号
2. 先在 vn.py 内部推一个 `Status.SUBMITTING`
3. 把 vn.py 的订单参数翻译成 FIX 下单参数，交给 `FixApplication`

核心调用是：

```python
self.application.send_new_order_single(
    symbol=req.symbol,
    side=fix_side_from_direction(req.direction),
    price=req.price,
    quantity=req.volume,
    cl_ord_id=orderid,
)
```

也就是说：

- vn.py 的 `Direction.LONG`
  - 会先变成 FIX Side `1`
- `price=10.5`
  - 变成 FIX `44=10.5`
- `volume=100`
  - 变成 FIX `38=100`

### 2.4 真正构造 FIX 消息对象

进入：

- `vnpy_quickfix_gateway/vnpy_quickfix_gateway/fix_application.py`
  - `FixApplication.send_new_order_single()`

这里创建的是 `fix42.NewOrderSingle()`，不是手搓字符串：

```python
message = fix42.NewOrderSingle()
message.setField(fix.ClOrdID(order_id))
message.setField(fix.HandlInst(fix.HandlInst_AUTOMATED_EXECUTION_NO_INTERVENTION))
message.setField(fix.Symbol(symbol))
message.setField(fix.Side(fix_side))
message.setField(fix.TransactTime())
message.setField(fix.OrderQty(quantity))
message.setField(fix.OrdType(fix.OrdType_LIMIT))
message.setField(fix.Price(price))
message.setField(fix.TimeInForce(fix.TimeInForce_DAY))
fix.Session.sendToTarget(message, self.session_id)
```

这里要特别注意：

- `fix42.NewOrderSingle()` 一创建，就已经自带：
  - `8=FIX.4.2`
  - `35=D`
- 你自己手工补的是：
  - `11 ClOrdID`
  - `21 HandlInst`
  - `55 Symbol`
  - `54 Side`
  - `60 TransactTime`
  - `38 OrderQty`
  - `40 OrdType`
  - `44 Price`
  - `59 TimeInForce`

### 2.5 QuickFIX C++ 内核如何把 Message 发成字符串

`fix.Session.sendToTarget(...)` 之后，进入 QuickFIX C++ 内核：

1. `Session::sendToTarget(...)`
2. `Session::send(...)`
3. `Session::sendRaw(...)`
4. `fill(header)` 自动补头字段
5. `Message::toString(...)` 序列化成最终字符串
6. `SocketConnection::send(...)` 发到 socket

这个过程中：

- `49=CLIENT1`
- `56=ORDERMATCH`
- `34=2`
- `52=20260709-02:18:05.105435`

都是 Session 层自动补的。

而：

- `9=165`
- `10=043`

则是 `Message::toString()` 自动计算的：

- `9` 是 BodyLength
- `10` 是 CheckSum

所以你当前项目里，真正“拼出最终 FIX 字符串”的不是 Python gateway，也不是 vn.py，而是 QuickFIX C++ 内核。

### 2.6 这条消息里每个字段是谁填的

还是看这条报文：

```text
8=FIX.4.2|9=165|35=D|34=2|49=CLIENT1|52=20260709-02:18:05.105435|56=ORDERMATCH|11=VN20260709021805104180|21=1|38=100|40=2|44=10.5|54=1|55=VNPY021805103315|59=0|60=20260709-02:18:05|10=043|
```

字段来源可以这样记：

- 由 `fix42.NewOrderSingle()` 自带：
  - `8=FIX.4.2`
  - `35=D`
- 由 Python `FixApplication.send_new_order_single()` 设置：
  - `11`
  - `21`
  - `38`
  - `40`
  - `44`
  - `54`
  - `55`
  - `59`
  - `60`
- 由 C++ Session 自动补：
  - `49`
  - `56`
  - `34`
  - `52`
- 由 C++ `Message::toString()` 自动算：
  - `9`
  - `10`

---

## 3. 链路 B：ordermatch 收到消息后如何解析

### 3.1 第一段：socket 字节流切成一条完整 FIX 报文

`ordermatch` 作为 acceptor 收到字节后，底层先走：

- `SocketConnection::readFromSocket()`
- `Parser::readFixMessage()`

`Parser::readFixMessage()` 的工作方式非常朴素：

1. 找 `8=`，确定报文起点
2. 找 `\x019=`，提取 `BodyLength`
3. 跳到正文末尾附近
4. 找 `\x0110=`，确定 checksum 字段
5. 找最后一个 `SOH`
6. 切出完整 FIX 报文

这也是你老师说的“`find` 多所以慢”的第一处典型表现。这里会大量做：

- `find("\0019=")`
- `find("\001")`
- `find("\00110=")`

### 3.2 第二段：原始字符串变成通用 Message

完整字符串切出来以后，进入：

- `Session::next(const std::string &msg, ...)`

这里会构造：

```cpp
Message(msg, sessionDD, validate)
```

真正解析在：

- `Message::setString(...)`

它会从头到尾扫描整条字符串，每遇到一个字段，就调用：

- `Message::extractField(...)`

`extractField(...)` 会做这些动作：

1. 用 `std::find(..., '=')` 找等号
2. 把等号前面的字符转成 tag 整数
3. 用 `std::find(..., '\001')` 找字段结束
4. 截出 value
5. 构造一个 `FieldBase(tag, value)`

对于示例里的 `NewOrderSingle`，它会依次拆出：

- `8 -> FIX.4.2`
- `9 -> 165`
- `35 -> D`
- `34 -> 2`
- `49 -> CLIENT1`
- `52 -> 20260709-02:18:05.105435`
- `56 -> ORDERMATCH`
- `11 -> VN20260709021805104180`
- `21 -> 1`
- `38 -> 100`
- `40 -> 2`
- `44 -> 10.5`
- `54 -> 1`
- `55 -> VNPY021805103315`
- `59 -> 0`
- `60 -> 20260709-02:18:05`
- `10 -> 043`

然后按字段类别分到 3 块：

- header
- body
- trailer

### 3.3 repeating group 在哪里解析

如果消息里有 repeating group，例如 `MarketDataRequest` 里的：

- `267=3`
- `269=0`
- `269=1`
- `269=2`

则 `Message::setString()` 会调用：

- `Message::setGroup(...)`

这里它会先去问数据字典：

- 这个 tag 在当前 `MsgType` 下是不是 group 计数字段
- 这个 group 的 delimiter 是哪个字段
- 这个 group 里允许哪些字段

然后把后面连续属于该 group 的字段收拢到 `Group` 对象里。

所以 QuickFIX 解析 group 不是简单“看到重复 tag 就当数组”，而是严格按 DataDictionary 定义去切。

### 3.4 数据字典校验做了什么

`Message` 拆出来以后，还要做 DataDictionary 校验。

主要逻辑在：

- `DataDictionary::validate(...)`

它会做这些检查：

- `BeginString` 是否和 session 匹配
- `MsgType` 是否存在
- 必填字段是否齐全
- 字段顺序是否合法
- `BodyLength` 是否正确
- `CheckSum` 是否正确
- 字段格式是否正确
- 字段值是否在允许枚举内
- repeating group 个数是否匹配

这里要特别注意：

- `validate()` 里已经会做“格式级别的类型检查”
- 例如 `44=10.5` 会调用 `PRICE_CONVERTOR::convert("10.5")`
- 但它只是验证“能不能转”
- 不会把结果缓存成后面业务层能直接复用的 `double`

### 3.5 如何进入 ordermatch 的 `onMessage(NewOrderSingle)`

Session 校验通过后，会进入应用回调：

- admin 消息 -> `fromAdmin`
- app 消息 -> `fromApp`

`ordermatch` 的入口是：

- `examples/ordermatch/Application.cpp`
  - `Application::fromApp(const FIX::Message&, const FIX::SessionID&)`

里面做的是：

```cpp
crack(message, sessionID);
```

这个 `crack()` 来自 `MessageCracker`。

它的分发逻辑是：

1. 先看 `8=FIX.4.2`
2. 决定走 `FIX42::MessageCracker`
3. 再看 `35=D`
4. 分发到：
   - `onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&)`

也就是说，`ordermatch` 这边是真正用到了 QuickFIX C++ 的 `MessageCracker`。

### 3.6 在 `onMessage(NewOrderSingle)` 里提取字段

进入：

- `Application::onMessage(const FIX42::NewOrderSingle&, ...)`

它会一项一项调用：

```cpp
message.getHeader().get(senderCompID);
message.getHeader().get(targetCompID);
message.get(clOrdID);
message.get(symbol);
message.get(side);
message.get(ordType);
message.get(price);
message.get(orderQty);
message.getFieldIfSet(timeInForce);
```

这一步会提取出：

- `senderCompID = CLIENT1`
- `targetCompID = ORDERMATCH`
- `clOrdID = VN20260709021805104180`
- `symbol = VNPY021805103315`
- `side = 1`
- `ordType = 2`
- `price = 10.5`
- `orderQty = 100`
- `timeInForce = DAY`

然后构造成 `ordermatch` 内部自己的业务对象：

- `Order`

它携带：

- clientId
- symbol
- owner
- target
- side
- type
- price
- quantity

### 3.7 业务处理：插入订单簿并撮合

进入：

- `Application::processOrder(const Order &order)`

逻辑是：

1. `m_orderMatcher.insert(order)`
2. `acceptOrder(order)` 先回一个 `NEW`
3. `m_orderMatcher.match(order.getSymbol(), orders)` 尝试撮合
4. 如果撮合成功，就对每个更新后的订单 `fillOrder(...)`

实际的价格时间匹配逻辑在：

- `examples/ordermatch/Market.cpp`

重点是：

- 最高买价和最低卖价相交时成交
- 成交价使用 `ask.getPrice()`
- 成交量取双方剩余量较小值
- 更新：
  - `openQuantity`
  - `executedQuantity`
  - `avgExecutedPrice`
  - `lastExecutedPrice`
  - `lastExecutedQuantity`

---

## 4. 链路 C：为什么多次 `message.get(...)` 会有重复 lookup

在 `ordermatch` 的 `onMessage(...)` 里你看到很多：

```cpp
message.get(symbol);
message.get(side);
message.get(price);
message.get(orderQty);
```

这些调用底层都会走：

- `FieldMap::getField(...)`
- `FieldMap::getFieldRef(tag)`
- `findTag(tag)`
- `lookup(...)`

而 `lookup(...)` 的策略是：

- 字段数小于 `16`
  - 走线性 `find_if`
- 字段数大于等于 `16`
  - 走 `lower_bound`

所以：

- `message.get(symbol)` 查一次
- `message.get(side)` 再查一次
- `message.get(price)` 再查一次

它们不会共享上一次 lookup 的结果。

但要注意：

- `message.getHeader().get(...)`
  - 只在 header 范围查
- `message.get(...)`
  - 只在 body 范围查

所以不是“整条报文每次都全扫一遍”，而是“各自分区里多次独立 lookup”。

### 4.1 为什么说 `setString()` 和 `validate()` 往往更值得先优化

虽然多次 `get(...)` 确实有重复查找，但通常更大的固定成本是：

1. `Message::setString()`
   - 要扫描整条原始字符串
   - 每个字段都要做：
     - 找 `=`
     - 找 `SOH`
     - 截字符串
     - 构造 `FieldBase`
   - 最后还要按 header/body/trailer 排序

2. `DataDictionary::validate()`
   - 对 header、body、trailer 全量遍历
   - 做：
     - 必填字段检查
     - tag 合法性检查
     - 格式检查
     - 枚举值检查
     - group 个数检查

而多次 `message.get(...)` 只是最后业务层按需取几个字段。

所以性能调优时，通常优先级会更像：

1. 原始报文解析和 FieldBase 构造
2. DataDictionary 校验
3. 业务层多次 lookup

### 4.2 `validate()` 里是不是已经做了类型转换

答案是：

- 做了“格式级别的类型检查”
- 但没有把转换结果缓存成业务层直接可复用的 native value

例如：

- `44=10.5`
  - 在 `validate()` 里会调用 `PRICE_CONVERTOR::convert("10.5")`
  - 但只是验证这串字符能不能当 Price
  - 不会把结果缓存成 `double`

所以当前 QuickFIX 经典路径会出现这种重复：

1. `validate()` 里 parse 一次，验证格式
2. `message.get(price)` 时做一次 lookup 和字符串拷贝
3. 真正把 `price` 当 `double` 用时，再 parse 一次

如果要进一步优化，就不是简单改一两行了，而是要做：

- 单次顺序遍历 + 同时校验 + 同时提取 + 同时转换
- 或者给 FieldBase 增加 typed cache

---

## 5. 链路 D：ordermatch 业务处理完后，如何发回 FIX 消息

### 5.1 ExecutionReport 是怎么构造出来的

在 `ordermatch` 里，订单接收后会进入：

- `Application::processOrder(...)`

如果插入成功，会：

1. `acceptOrder(order)` 发一个 `NEW`
2. `match(...)` 做撮合
3. `fillOrder(order)` 发 `FILL` 或 `PARTIAL_FILL`

真正构造 `ExecutionReport` 的函数是：

- `Application::updateOrder(const Order &order, char status)`

这里创建：

```cpp
FIX42::ExecutionReport fixOrder(
    FIX::OrderID(order.getClientID()),
    FIX::ExecID(m_generator.genExecutionID()),
    FIX::ExecTransType(FIX::ExecTransType_NEW),
    FIX::ExecType(status),
    FIX::OrdStatus(status),
    FIX::Symbol(order.getSymbol()),
    FIX::Side(convert(order.getSide())),
    FIX::LeavesQty(order.getOpenQuantity()),
    FIX::CumQty(order.getExecutedQuantity()),
    FIX::AvgPx(order.getAvgExecutedPrice()));
```

然后补：

- `ClOrdID`
- `OrderQty`

如果是成交型状态，还会补：

- `LastShares`
- `LastPx`

所以 `ExecutionReport` 的业务字段主要来自 `Order` 当前状态：

- 剩余量
- 累计成交量
- 均价
- 最新成交价
- 最新成交量

### 5.2 发送给客户端时谁负责补哪些字段

`updateOrder()` 最后调用：

```cpp
FIX::Session::sendToTarget(fixOrder, senderCompID, targetCompID);
```

这里：

- `senderCompID = ORDERMATCH`
- `targetCompID = CLIENT1`

QuickFIX C++ 之后自动补：

- `49=ORDERMATCH`
- `56=CLIENT1`
- `34=...`
- `52=...`
- `9=...`
- `10=...`

所以 server 端的分工和 client 发单时完全对称：

- 业务应用层只负责业务字段
- QuickFIX 内核负责会话字段、序列化、网络发送

### 5.3 MarketDataSnapshot 是怎么构造出来的

行情回报在：

- `Application::sendMarketDataSnapshot(...)`

这里会构造：

- `FIX42::MarketDataSnapshotFullRefresh snapshot(symbol)`

然后设置：

- `MDReqID`

再通过 `addMarketDataEntry(...)` 加 3 个 repeating group：

1. `BID`
2. `OFFER`
3. `TRADE`

也就是你后来看到的这种回包结构：

```text
268=3
269=0 270=10   271=100
269=1 270=10.5 271=100
269=2 270=10.25 271=10
```

最后同样调用：

```cpp
FIX::Session::sendToTarget(snapshot, sessionID);
```

---

## 6. 链路 E：vnpy 这边如何接收 FIX 回报并解析

### 6.1 QuickFIX C++ 收到 server 回包后的底层路径

虽然你这边是 Python 程序，但底层接收流程第一段仍然是 QuickFIX C++ 在做：

1. `SocketConnection::readFromSocket()`
2. `Parser::readFixMessage()`
3. `Session::next(raw_string, now)`
4. `Message(raw_string, sessionDD, validate)` 构造成通用 `Message`
5. `DataDictionary::validate(...)`
6. `Session::verify(...)`
7. `Session::fromCallback(...)`

如果是应用层消息：

- `ExecutionReport(35=8)`
- `MarketDataSnapshotFullRefresh(35=W)`

都会进入 `fromApp(...)`。

### 6.2 C++ 是怎么把回调抛到 Python 的

这里的桥接层是 SWIG 生成的 director。

关键入口在：

- `src/python3/QuickfixPython.cpp`
  - `SwigDirector_Application::fromApp(...)`

也就是说：

1. C++ 内核已经把消息解析成 `FIX::Message`
2. 通过 SWIG director
3. 回调到你 Python 里继承的 `fix.Application`

所以你自己的 Python 回调入口是：

- `vnpy_quickfix_gateway/vnpy_quickfix_gateway/fix_application.py`
  - `FixApplication.fromApp(...)`

### 6.3 Python `FixApplication.fromApp()` 怎么分发

在：

- `FixApplication.fromApp(...)`

里先做：

```python
msg_type = get_msg_type(message)
```

再按 `35` 手工分派：

- `35=8`
  - `self.on_execution_report(message)`
- `35=W`
  - `self.on_market_data_snapshot(message)`

这里和 `ordermatch` 不同：

- `ordermatch`
  - 用的是 C++ `MessageCracker`
- `vnpy_quickfix_gateway`
  - 当前用的是 Python 侧手工 `if/elif` 分发

### 6.4 ExecutionReport 在 Python 端如何解析

进入：

- `FixApplication.on_execution_report()`

这里会调用：

- `mapping.execution_report_from_fix(message)`

解析函数会依次提取：

- `OrderID`
- `ClOrdID`
- `ExecID`
- `ExecType`
- `OrdStatus`
- `Symbol`
- `Side`
- `OrderQty`
- `LeavesQty`
- `CumQty`
- `AvgPx`
- `LastShares` / `LastQty`
- `LastPx`
- `Text`

得到一个归一化 dataclass：

- `FixExecutionReport`

然后 `gateway.py` 里的：

- `QuickfixGateway.on_fix_execution_report(report)`

会把它进一步翻译成 vn.py 里的：

- `OrderData`
- `TradeData`

最后分别调用：

- `self.on_order(order)`
- `self.on_trade(trade)`

### 6.5 MarketDataSnapshot 在 Python 端如何解析

进入：

- `FixApplication.on_market_data_snapshot()`

再调用：

- `mapping.market_data_snapshot_from_fix(message)`

这里会：

1. 先读：
   - `Symbol`
   - `MDReqID`
   - `NoMDEntries`
2. 创建：
   - `fix42.MarketDataSnapshotFullRefresh.NoMDEntries()`
3. 循环 `message.getGroup(index, group)`
4. 每个 group 里提取：
   - `MDEntryType`
   - `MDEntryPx`
   - `MDEntrySize`
5. 按 entry type 分类成：
   - `bid_price_1`
   - `bid_volume_1`
   - `ask_price_1`
   - `ask_volume_1`
   - `last_price`
   - `last_volume`

然后在 `gateway.py` 里：

- `tick_data_from_market_data_snapshot(snapshot)`

翻译成 vn.py 的 `TickData`，再：

- `self.on_tick(tick)`

### 6.6 最后怎么进 vn.py GUI

`QuickfixGateway` 继承的是 vn.py 的 `BaseGateway`。

所以：

- `self.on_order(order)`
- `self.on_trade(trade)`
- `self.on_tick(tick)`

最终都会进入：

- `BaseGateway.on_event(...)`
- `event_engine.put(Event(...))`

所以 GUI 看到的更新，实际上来自：

1. QuickFIX 回报 -> Python message
2. Python message -> vn.py 对象
3. vn.py 对象 -> EventEngine
4. EventEngine -> GUI 组件刷新

---

## 7. 链路 F：server 回报到 vn.py GUI 的一条完整闭环

以一笔双边撮合后的成交回报为例，可以把整条闭环压成下面这样：

1. vn.py 发送 `NewOrderSingle`
2. QuickFIX C++ 发到 socket
3. `ordermatch` 收到并解析
4. `ordermatch` 构造内部 `Order`
5. `OrderMatcher` 撮合
6. `Application::updateOrder()` 构造 `FIX42::ExecutionReport`
7. `Session::sendToTarget()` 发回 client
8. client 侧 QuickFIX C++ 解析回 `FIX::Message`
9. SWIG director 回调到 Python `FixApplication.fromApp()`
10. `execution_report_from_fix()` 提取字段
11. `on_fix_execution_report()` 翻译成 vn.py `OrderData` / `TradeData`
12. `BaseGateway.on_order()` / `on_trade()`
13. `EventEngine` 推事件
14. GUI 刷新订单和成交

行情快照 `35=W` 也是同一条大链路，只是中间第 10 到 12 步换成：

- `market_data_snapshot_from_fix()`
- `tick_data_from_market_data_snapshot()`
- `on_tick()`

---

## 8. QuickFIX 自带 example：tradeclient 和 executor 命令行

如果你想单独跑 QuickFIX 官方 example，而不经过 vn.py / gateway，可以直接使用：

### 8.1 本机回环配置

终端 1，启动 `executor`：

```bash
cd /path/to/vnpy-quickfix-acrl/QuickFIX/bin
./executor cfg/executor_local.cfg
```

终端 2，启动 `tradeclient`：

```bash
cd /path/to/vnpy-quickfix-acrl/QuickFIX/bin
./tradeclient cfg/tradeclient_local.cfg
```

### 8.2 `tradeclient` 菜单

启动后菜单类似：

```text
1) Enter Order
2) Cancel Order
3) Replace Order
4) Market data test
5) Quit
Action:
```

注意：

- `tradeclient_local.cfg` / `executor_local.cfg` 是 `FIX.4.2`
- 所以适合测试：
  - 下单
  - 撤单
  - 改单
- `Market data test`
  - example 代码主要给 `FIX.4.3/4.4/5.0`
  - 不适合这组 `FIX.4.2 local` 配置

### 8.3 一套最小下单示例

依次输入：

```text
Action: 1
BeginString: 3
ClOrdID: T001
Symbol: AAPL
Side: 1
OrdType: 2
OrderQty: 100
TimeInForce: 1
Price: 10.5
SenderCompID: CLIENT1
TargetCompID: EXECUTOR
Use a TargetSubID?: N
Send order?: Y
```

这里：

- `BeginString: 3`
  - 对应 `FIX.4.2`
- `Side: 1`
  - Buy
- `OrdType: 2`
  - Limit
- `TimeInForce: 1`
  - Day

### 8.4 一套最小撤单示例

```text
Action: 2
BeginString: 3
OrigClOrdID: T001
ClOrdID: C001
Symbol: AAPL
Side: 1
OrderQty: 100
SenderCompID: CLIENT1
TargetCompID: EXECUTOR
Use a TargetSubID?: N
Send cancel?: Y
```

### 8.5 一套最小改单示例

```text
Action: 3
BeginString: 3
OrigClOrdID: T001
ClOrdID: R001
Symbol: AAPL
Side: 1
OrdType: 2
OrderQty: 120
TimeInForce: 1
Price: 10.8
SenderCompID: CLIENT1
TargetCompID: EXECUTOR
Use a TargetSubID?: N
Send replace?: Y
```

---

## 9. 最后给你一个总记忆法

你可以把整个项目强行压缩成下面这 4 句：

1. vn.py 负责产生交易语义
2. gateway 负责把交易语义翻译成 FIX 语义，再把 FIX 回报翻译回 vn.py 语义
3. QuickFIX C++ 内核负责 FIX Message 和网络字节流之间的转换
4. ordermatch 负责服务端业务逻辑和撮合

如果后面你开始做性能优化，也可以先按这 3 层去看热点：

1. 解析层
   - `Parser`
   - `Message::setString()`
   - `FieldBase`
2. 校验层
   - `DataDictionary::validate()`
3. 业务提取层
   - `message.get(...)`
   - `mapping.py`

这样你再回头看源码时，就不会只看到“很多类、很多函数”，而是能很明确地知道：这段代码到底是在做业务翻译、协议解析、还是网络传输。
