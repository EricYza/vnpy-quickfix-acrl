# QuickFIX 第二阶段导读：消息对象和协议层

这份文档只做第二阶段的事情。

这一阶段我们不再停留在“示例程序怎么把 QuickFIX 跑起来”，而是往下一层看：

1. `FIX::Message` 和 `FIX::FieldMap` 这种通用消息容器到底是什么。
2. `FIX42::NewOrderSingle`、`FIX42::ExecutionReport` 这种强类型消息类是怎么来的。
3. `spec/FIX42.xml` 和生成出来的 C++ 头文件之间是什么关系。
4. 为什么收到一条 `35=D` 的 FIX42 消息后，`crack()` 能自动走到 `onMessage(const FIX42::NewOrderSingle&, ...)`。

这份文档聚焦你指定的这些文件：

- `src/C++/Message.h`
- `src/C++/FieldMap.h`
- `src/C++/fix42/MessageCracker.h`
- `src/C++/fix42/NewOrderSingle.h`
- `src/C++/fix42/ExecutionReport.h`
- `spec/FIX42.xml`

同时我会补一点必要的旁支文件，帮助你把生成链路讲完整：

- `src/C++/fix42/Message.h`
- `spec/Generator.rb`
- `spec/GeneratorCPP.rb`
- `spec/MessageCracker.xsl`
- `spec/generate.sh`

原因很简单：

- 只看结果文件，你能“看见长相”
- 但看不到“为什么会长成这样”

而你这一阶段真正要建立的是：

> 协议定义 -> 代码生成 -> 强类型消息类 -> 消息分发

这整条因果链。

---

## 1. 先说这一阶段到底要得到什么感觉

第一阶段你主要是在学“怎么用 QuickFIX”。

第二阶段你要建立的感觉是：

1. 一个 FIX 消息在内存里，本质上还是字段集合。
2. `FIX42::NewOrderSingle` 这种类，不是“另一套完全独立的数据结构”，而是“对通用消息容器的强类型包装”。
3. `FIX42.xml` 这种协议文件，不只是文档，而是代码生成输入。
4. `35=D` 自动变成 `FIX42::NewOrderSingle`，靠的不是神秘反射，而是生成好的分发代码。

如果把这一阶段压缩成一句话：

> QuickFIX 的强类型消息世界，本质上是“协议 XML 驱动生成出来的一层类型安全外壳”，底层仍然是通用的 `FieldMap + Message` 结构。

---

## 2. 先从最底层看：字段、字段集合、消息，这三层怎么叠起来

这一阶段最容易迷路的地方，是把“字段类”“消息类”“协议类”混在一起。

先把最基础的三层拆开：

1. 字段
   例如 `ClOrdID`、`Symbol`、`Price`

2. 字段集合
   一堆字段放在一起，形成 Header、Body、Trailer、Group

3. 消息
   Header + Body + Trailer 组成一条完整 FIX 报文

---

## 3. `FieldMap.h`：它是 QuickFIX 里最底层的“字段容器”

如果你只记一句话：

> `FieldMap` 就是一个“能保存 FIX 字段和重复组的容器”。

### 3.1 `FieldMap` 存的核心数据是什么

在 `src/C++/FieldMap.h` 里，最关键的两个成员是：

- `Fields m_fields`
- `Groups m_groups`

也就是：

- 普通字段列表
- 重复组集合

更具体地说：

- `Fields` 是 `std::vector<FieldBase>`
- `Groups` 是按 tag 编号组织的 group 容器

这说明 `FieldMap` 的本质不是“哈希表式消息对象”，而是：

- 一组带顺序的字段
- 外加按组管理的 repeating group

### 3.2 为什么它不是简单 `map<int, string>`

因为 FIX 不只是“tag 到 value”的字典。

它还关心：

- 字段顺序
- 重复组
- BodyLength / CheckSum 计算
- 某些场景下的编码还原

所以 `FieldMap` 不能只做最简单的键值映射。

它要兼顾：

1. 能按 tag 查字段
2. 能尽量保持 FIX 输出顺序
3. 能容纳 group
4. 能算 length 和 checksum

### 3.3 `FieldMap` 里最常用的能力

它提供了几个你读消息类时会反复见到的能力：

- `setField(...)`
- `getField(...)`
- `getFieldIfSet(...)`
- `isSetField(...)`
- `addGroup(...)`
- `getGroup(...)`
- `removeGroup(...)`

这几乎就是 FIX 消息在内存中的基本操作集。

### 3.4 `FieldMap` 为什么要排序

在 `FieldMap` 里你会看到：

- `message_order`
- `sorter`
- `findPositionFor(...)`
- `sortFields()`

这说明 `FieldMap` 不是随便乱放字段。

它会尽量按照 FIX 期望的字段顺序来组织。

这很重要，因为 FIX 报文在序列化输出时，字段顺序不是完全无所谓的。

所以你可以这样理解：

- `FieldMap` 是“有顺序意识的字段容器”

### 3.5 `FieldMap` 不只管字段，还管 repeating group

例如：

- `NoAllocs`
- `NoContraBrokers`

这些在 FIX 里不是单个字段，而是“组计数字段 + 多组子字段”。

`FieldMap` 里单独有：

- `addGroup`
- `getGroup`
- `groupCount`

这正是为 repeating group 准备的。

### 3.6 `FIELD_SET` 宏是消息类接口的关键

在 `FieldMap.h` 结尾，你会看到这个宏：

```cpp
#define FIELD_SET(MAP, FIELD) ...
```

它会生成四个常用方法：

- `isSet(...)`
- `set(...)`
- `get(...)`
- `getIfSet(...)`

这件事非常关键，因为后面你看到的所有：

```cpp
FIELD_SET(*this, FIX::ClOrdID);
FIELD_SET(*this, FIX::Symbol);
FIELD_SET(*this, FIX::Price);
```

本质上都是在说：

- “请为这个消息类生成针对该字段的强类型访问接口”

所以 `FIELD_SET` 是“通用容器”变成“好用消息类”的桥之一。

### 3.7 `FieldMap` 在消息层里的地位

你可以先记住一个非常重要的关系：

- `Header` 是 `FieldMap`
- `Trailer` 是 `FieldMap`
- `Message` 本体也继承自 `FieldMap`

也就是说：

> 整个 FIX 消息系统，底层都是 `FieldMap` 在托底。

---

## 4. `Message.h`：把 Header、Body、Trailer 组合成一条完整 FIX 消息

如果 `FieldMap` 是“字段容器”，那 `Message` 就是“完整报文容器”。

### 4.1 `Message` 的核心结构

在 `src/C++/Message.h` 里最关键的一句说明其实已经写出来了：

> A message consists of three field maps. One for the header, the body, and the trailer.

也就是：

- Header
- Body
- Trailer

三段都是字段集合。

### 4.2 `Message` 为什么继承自 `FieldMap`

因为消息体 body 本身就是一组字段。

所以 QuickFIX 的做法是：

- `Message` 自己继承 `FieldMap`
- 再额外持有 `m_header`
- 再额外持有 `m_trailer`

于是就形成了：

- `m_header`
- `FieldMap` 这部分本体
- `m_trailer`

这三个部分正好对应一条 FIX 消息的三个逻辑区域。

### 4.3 `Message` 的几个非常关键的能力

它除了继承 `FieldMap` 的 set/get/group 能力，还额外提供：

- `toString()`
- `toXML()`
- `setString(...)`
- `setStringHeader(...)`
- `getHeader()`
- `getTrailer()`
- `isAdmin()`
- `isApp()`
- `getSessionID()`
- `setSessionID()`

这意味着：

- 它既能表示内存中的消息对象
- 也能从字符串解析消息
- 还能把消息再编码成字符串/XML

### 4.4 `Message` 为什么既能“构造新消息”，又能“解析收到的消息”

因为它有两类构造方式：

1. 由代码构造
   例如通过 `Message(const BeginString&, const MsgType&)`

2. 由字符串构造
   例如 `Message(const std::string&, ...)`

这正好对应两个场景：

- 出站消息：程序自己创建
- 入站消息：从 socket 收到字符串后解析

### 4.5 `Message(const BeginString&, const MsgType&)` 很关键

这个 protected 构造函数是所有强类型消息类的起点。

因为它意味着：

- 任何派生消息类，只要知道自己的 `BeginString` 和 `MsgType`
- 就能建立一个合法的空白消息骨架

这会直接和 `FIX42::Message`、`FIX42::NewOrderSingle` 连起来。

### 4.6 `Message` 并不强绑定某个 FIX 版本

通用 `FIX::Message` 并不知道自己一定是 FIX4.2 或 FIX4.4。

它只是一个：

- 能放 header/body/trailer 的通用消息对象

至于：

- BeginString 是 `FIX.4.2`
- MsgType 是 `D`

这些更具体的信息，是更上层版本化消息类补进去的。

### 4.7 `identifyType()` 暗示了 35 很核心

`Message.h` 结尾有个小工具：

```cpp
inline MsgType identifyType(const std::string &message)
```

它做的就是从原始 FIX 字符串里快速找出：

- `35=...`

这说明在整个 FIX 世界里：

- `35=MsgType` 是消息类型分发的核心入口

这一点后面讲 `MessageCracker` 时会直接用上。

---

## 5. `FIX42::Message`：在通用消息之上固定住 FIX4.2 版本

这一点虽然不在你最初列的必读清单里，但它正好卡在 `Message.h` 和 `NewOrderSingle.h` 之间，所以必须补上。

文件是：

- `src/C++/fix42/Message.h`

### 5.1 它解决什么问题

通用 `FIX::Message` 不知道自己属于哪个 FIX 版本。

而 `FIX42::Message` 的作用就是：

- 把 `BeginString("FIX.4.2")` 固定进去

它最关键的构造函数大意是：

```cpp
Message(const FIX::MsgType& msgtype)
  : FIX::Message(FIX::BeginString("FIX.4.2"), msgtype) {}
```

这意味着：

- 只要你构造一个 `FIX42::Message`
- 它天然就是 FIX4.2 版本消息

### 5.2 它还生成了版本化 Header 和 Trailer

`FIX42::Message.h` 里不只是版本构造函数。

它还生成了：

- `FIX42::Header`
- `FIX42::Trailer`

这些类内部也会通过 `FIELD_SET` 提供：

- `BeginString`
- `BodyLength`
- `MsgType`
- `SenderCompID`
- `TargetCompID`
- `CheckSum`

等 FIX4.2 版本头尾字段接口。

所以 `FIX42::Message` 的意义是：

1. 固定协议版本
2. 提供版本化 header/trailer 类型接口

---

## 6. 强类型消息类到底是什么：先看 `FIX42::NewOrderSingle`

这就是第二阶段第一个核心目标。

文件：

- `src/C++/fix42/NewOrderSingle.h`

### 6.1 第一眼先看三件事

这个文件里最值得先看的，不是整页的 `FIELD_SET`，而是这三件事：

1. 它继承谁
2. 它的 `MsgType()` 是多少
3. 它的构造函数参数有哪些

### 6.2 它继承的是 `FIX42::Message`

类声明是：

```cpp
class NewOrderSingle : public Message
```

注意这里的 `Message` 指的是 `FIX42::Message`，不是通用 `FIX::Message`。

这很重要，因为它说明：

- 这个类天然属于 FIX4.2 命名空间
- 天然带着 `BeginString=FIX.4.2`

### 6.3 它把自己的消息类型固定成 `"D"`

你会看到：

```cpp
static FIX::MsgType MsgType() { return FIX::MsgType("D"); }
```

这就是：

- `35=D`

在代码里的固定来源。

所以当你写：

```cpp
FIX42::NewOrderSingle order(...);
```

本质上你是在创建一个：

- BeginString 已固定为 `FIX.4.2`
- MsgType 已固定为 `D`

的消息对象。

### 6.4 它的必填构造函数来自 XML 的 required 字段

这是整个第二阶段最重要的“生成链路认知”之一。

在 `FIX42.xml` 里，`NewOrderSingle` 的定义是：

```xml
<message name='NewOrderSingle' msgtype='D' msgcat='app'>
  <field name='ClOrdID' required='Y' />
  ...
  <field name='HandlInst' required='Y' />
  ...
  <field name='Symbol' required='Y' />
  ...
  <field name='Side' required='Y' />
  ...
  <field name='TransactTime' required='Y' />
  ...
  <field name='OrdType' required='Y' />
  ...
</message>
```

而生成出来的 C++ 构造函数正好是：

```cpp
NewOrderSingle(
  const FIX::ClOrdID& aClOrdID,
  const FIX::HandlInst& aHandlInst,
  const FIX::Symbol& aSymbol,
  const FIX::Side& aSide,
  const FIX::TransactTime& aTransactTime,
  const FIX::OrdType& aOrdType )
```

这不是巧合，而是生成器明确这样做的。

### 6.5 生成器为什么会这样生成

关键逻辑在：

- `spec/Processor.rb`
- `spec/GeneratorCPP.rb`

`Processor.rb` 在处理 message 时，会收集：

```ruby
message.elements.each("field[@required='Y']")
```

也就是：

- 把 XML 里所有 `required='Y'` 的字段名收集出来

然后交给 `GeneratorCPP#messageStart(...)`。

`GeneratorCPP.rb` 再用这个 `required` 列表生成：

1. 构造函数参数列表
2. 构造函数体里对应的 `set(aField)`

所以整个逻辑链是：

```text
FIX42.xml 里的 required='Y'
-> Processor.rb 收集必填字段
-> GeneratorCPP.rb 生成消息类构造函数
-> NewOrderSingle.h 里出现对应参数和 set 调用
```

这就是“强类型消息类怎么从协议定义长出来”的第一层答案。

### 6.6 `FIELD_SET(*this, FIX::ClOrdID)` 在干什么

这类生成代码非常多，例如：

```cpp
FIELD_SET(*this, FIX::ClOrdID);
FIELD_SET(*this, FIX::Symbol);
FIELD_SET(*this, FIX::Side);
FIELD_SET(*this, FIX::OrdType);
FIELD_SET(*this, FIX::Price);
```

它们本质上是在给这个消息类生成：

- `set(const FIX::ClOrdID&)`
- `get(FIX::ClOrdID&)`
- `isSet(const FIX::ClOrdID&)`
- `getIfSet(FIX::ClOrdID&)`

所以 `NewOrderSingle` 并不是自己单独存了一个 `m_clOrdID` 成员变量。

它只是通过 `FieldMap` 存储底层字段，再用这些生成方法让访问变得像“有成员字段”一样自然。

### 6.7 它为什么看起来像“有类型”，底层却仍是字段集合

这是第二阶段非常关键的认知：

- `NewOrderSingle` 没有自己额外声明一堆成员变量
- 它主要只是继承 `Message`，然后生成很多访问方法

这意味着：

> `FIX42::NewOrderSingle` 更像是“带强类型接口的消息视图”，不是另一套完全独立的数据结构。

### 6.8 repeating group 也是从 XML 长出来的

在 `FIX42.xml` 里，`NewOrderSingle` 里有：

```xml
<group name='NoAllocs' required='N'>
  <field name='AllocAccount' required='N' />
  <field name='AllocShares' required='N' />
</group>
```

对应生成出的 C++ 里会有：

```cpp
class NoAllocs: public FIX::Group
{
public:
  NoAllocs() : FIX::Group(78,79,FIX::message_order(79,80,0)) {}
  FIELD_SET(*this, FIX::AllocAccount);
  FIELD_SET(*this, FIX::AllocShares);
};
```

这里有两层非常重要的信息：

1. `NoAllocs` 不是手写的，是 XML group 生成的
2. 组里的 delim 和字段顺序，也是根据 XML 算出来的

这说明生成器不只是会生成简单字段，还会把 repeating group 结构也映射成类。

---

## 7. 再看 `FIX42::ExecutionReport`：它和 XML 的关系同样直接

文件：

- `src/C++/fix42/ExecutionReport.h`

### 7.1 先看它的 `MsgType()`

你会看到：

```cpp
static FIX::MsgType MsgType() { return FIX::MsgType("8"); }
```

这就是：

- `35=8`

也就是 FIX 里的 `ExecutionReport`。

### 7.2 它的必填构造函数同样直接来自 XML

`FIX42.xml` 里 `ExecutionReport` 的 required 字段包括：

- `OrderID`
- `ExecID`
- `ExecTransType`
- `ExecType`
- `OrdStatus`
- `Symbol`
- `Side`
- `LeavesQty`
- `CumQty`
- `AvgPx`

而生成出来的构造函数正好就是这 10 个参数。

这进一步证明：

- 生成器不是“猜一个差不多的构造函数”
- 而是在严格读取 XML 的 required 标记

### 7.3 为什么你在示例程序里还会看到额外 `set(...)`

例如 `executor` 里构造 `ExecutionReport` 后，还会再补：

- `ClOrdID`
- `OrderQty`
- `LastShares`
- `LastPx`

这是因为：

- 构造函数只负责协议定义里的必填字段
- 可选字段仍然通过 `set(...)` 继续加

所以 QuickFIX 的使用方式很清晰：

1. 必填字段走构造函数
2. 可选字段走 `set(...)`

### 7.4 `ExecutionReport` 里的 group 也来自 XML

例如 `NoContraBrokers`。

在 XML 里是：

```xml
<group name='NoContraBrokers' required='N'>
  ...
</group>
```

在生成头文件里就会出现：

```cpp
class NoContraBrokers: public FIX::Group
```

所以消息类、group 类、必填构造函数、字段访问器，都是同一条 XML 生成链上长出来的。

---

## 8. `FIX::ClOrdID`、`FIX::OrdType` 这些字段类型从哪来

读到这里，很多人会有个自然疑问：

`NewOrderSingle.h` 里这些类型：

- `FIX::ClOrdID`
- `FIX::HandlInst`
- `FIX::Symbol`
- `FIX::OrdType`

又是从哪来的？

### 8.1 它们本质上是“字段类”

这些类的底层基础在：

- `src/C++/Field.h`

这里定义了很多宏，比如：

- `DEFINE_STRING`
- `DEFINE_CHAR`
- `DEFINE_PRICE`
- `DEFINE_QTY`
- `DEFINE_UTCTIMESTAMP`

它们用来生成字段类。

### 8.2 具体字段声明在 `FixFields.h` / `FixCommonFields.h`

例如：

- `DEFINE_STRING(ClOrdID);`
- `DEFINE_CHAR(OrdType);`
- `DEFINE_CHAR(ExecType);`
- `DEFINE_CHAR(OrdStatus);`

这意味着：

- `FIX::ClOrdID` 是一个“tag=11 的字符串字段类”
- `FIX::OrdType` 是一个对应 tag 的字符字段类

所以你看到的这些“类型名很像业务字段”的对象，本质上不是手写业务结构，而是：

- 由字段宏定义出来的强类型字段包装器

### 8.3 这说明强类型其实分两层

QuickFIX 的“强类型”不是只有消息层。

它至少有两层：

1. 强类型字段
   例如 `FIX::ClOrdID`、`FIX::Price`

2. 强类型消息
   例如 `FIX42::NewOrderSingle`、`FIX42::ExecutionReport`

你可以理解为：

- 字段类是积木
- 消息类是按协议把积木拼起来的结果

---

## 9. `FIX42.xml` 到底是什么地位

这一阶段必须把 `spec/FIX42.xml` 的角色看清楚。

它不是“仅供人阅读的协议说明书”。

它更准确地说是：

- QuickFIX 代码生成和校验体系的重要输入

### 9.1 这个 XML 里定义了什么

至少包括：

- 消息列表
- 每个消息的 `msgtype`
- 每个字段是否 required
- group 结构
- 各字段的定义
- 枚举值

例如你已经看到了：

```xml
<message name='NewOrderSingle' msgtype='D' msgcat='app'>
```

和：

```xml
<message name='ExecutionReport' msgtype='8' msgcat='app'>
```

这两行已经足够决定很多后续生成结果。

### 9.2 `msgtype='D'` 和 `name='NewOrderSingle'` 会影响什么

至少影响两类生成物：

1. `NewOrderSingle.h`
   里会生成：
   - 类名 `NewOrderSingle`
   - `MsgType()` 返回 `"D"`

2. `MessageCracker.h`
   里会生成：
   - `if (msgTypeValue == "D") return onMessage((const NewOrderSingle&)message, sessionID);`

所以：

- `name`
  决定类名和函数名
- `msgtype`
  决定分发条件和消息类型常量

### 9.3 `required='Y'` 会影响什么

它会直接影响：

- 构造函数参数列表

例如 `NewOrderSingle` 的 6 个必填字段，都是 XML 里 required='Y' 的字段。

### 9.4 `<group ...>` 会影响什么

它会直接影响：

- 内嵌 group 类生成
- group 的计数字段
- delim 字段
- group 内字段顺序

### 9.5 `FIX42.xml` 不是最终源头，但它是当前仓库里的直接输入

如果再往上追一层，`spec/generate_spec.rb` 说明：

- 这些 `FIX42.xml` 又是从更大的统一 FIX 仓库整理出来的

但对你当前理解 QuickFIX 源码来说，更直接的链路是：

```text
spec/FIX42.xml
-> Generator / XSLT
-> src/C++/fix42/*.h
```

这条链已经足够解释当前项目结构。

---

## 10. 生成链路：消息类和 MessageCracker 不是同一种工具生成的

这点很容易忽略，但其实很有意思。

### 10.1 `spec/generate.sh` 干了两件事

它内容很短：

```bash
./generate_c++.sh
ruby -I. Generator.rb
```

也就是说它分两段：

1. 先跑 shell/XSLT
2. 再跑 Ruby 生成器

### 10.2 `MessageCracker.h` 是 XSLT 生成的

`spec/generate_c++.sh` 里有：

```bash
xsltproc -o ../src/C++/fix42/MessageCracker.h MessageCracker.xsl FIX42.xml
```

这说明：

- `fix42/MessageCracker.h` 是 `MessageCracker.xsl + FIX42.xml` 生成的

### 10.3 `NewOrderSingle.h` / `ExecutionReport.h` 是 Ruby 生成的

`spec/Generator.rb` 会创建：

- `GeneratorCPP`
- `Processor`

然后处理 XML。

`GeneratorCPP.rb` 里真正生成：

- 版本 `Message.h`
- 各条消息的 `.h`
- 字段定义相关文件

所以：

- `MessageCracker.h` 更偏 XSLT 生成
- `NewOrderSingle.h` / `ExecutionReport.h` 更偏 Ruby 生成

### 10.4 为什么项目会这样混搭

从工程历史感来看，很像是：

- 有些文本模板适合 XSLT 做
- 有些结构化 C++ 头文件更适合 Ruby 做

你不需要纠结这是不是“最佳现代方案”，但要知道：

> 这个项目的自动生成体系是分层且混合的。

---

## 11. 现在回答第一核心问题：强类型消息类是怎么从协议定义长出来的

这一题我们现在可以完整回答了。

### 11.1 第一步：协议定义写在 `FIX42.xml`

以 `NewOrderSingle` 为例，它定义了：

- 这条消息叫什么名字：`NewOrderSingle`
- 它的 `msgtype` 是 `D`
- 哪些字段必填
- 哪些字段可选
- 哪些 repeating group 存在

### 11.2 第二步：`Processor.rb` 读取 XML

它会：

1. 遍历每条 `<message>`
2. 收集 message 的字段
3. 收集 `required='Y'` 的字段
4. 收集 group 定义

### 11.3 第三步：`GeneratorCPP.rb` 生成对应消息头文件

它会按 XML 内容生成：

1. 类名
2. `MsgType()` 静态函数
3. 默认构造函数
4. 必填字段构造函数
5. `FIELD_SET(...)` 字段访问器
6. group 内嵌类

### 11.4 第四步：得到像 `FIX42::NewOrderSingle` 这样的类

于是你最终看到：

- 这是 FIX4.2 消息
- 它的类型码是 D
- 它必填的 6 个字段直接进构造函数
- 它可选字段通过 `set(...)` 添加
- 它的 repeating group 也变成了类

所以所谓“强类型消息类从协议定义长出来”，不是比喻，几乎是字面意义：

- XML 描述结构
- 生成器把结构翻译成 C++ 类型

### 11.5 这一层“强类型”的真实含义

它并不是说消息内部真的有：

- `m_clOrdID`
- `m_symbol`
- `m_side`

这种一一对应的成员变量。

它真正的含义是：

- 给通用消息容器套上一层类型安全、字段名友好的接口

所以你既能保持 FIX 的灵活字段结构，又能得到像普通 C++ 类型那样的写法。

---

## 12. 现在回答第二核心问题：`35=D` 为什么能自动变成 `FIX42::NewOrderSingle`

这是本阶段第二个核心目标。

### 12.1 先把“自动变成”这句话说准确

更准确的说法其实是：

> QuickFIX 会先把消息当成通用消息容器处理，然后依据版本和 `MsgType`，把它分发成 `NewOrderSingle` 这个强类型接口来调用你的 `onMessage(...)`。

这和“动态创建了一个全新的对象”不是一回事。

这个区别非常重要。

### 12.2 分发的第一层：按 FIX 版本

公共的 `FIX::MessageCracker` 会先看：

- Header 里的 `BeginString`

如果它是：

- `FIX.4.2`

那么它会把消息交给：

- `FIX42::MessageCracker`

所以第一层逻辑是：

```text
BeginString=FIX.4.2
-> 交给 FIX42 版本的分发器
```

### 12.3 分发的第二层：按 `35=MsgType`

到了 `src/C++/fix42/MessageCracker.h` 里，生成代码大意是：

```cpp
const std::string & msgTypeValue = message.getHeader().getField(FIX::FIELD::MsgType);

if( msgTypeValue == "D" )
  return onMessage( (const NewOrderSingle&)message, sessionID );

if( msgTypeValue == "8" )
  return onMessage( (const ExecutionReport&)message, sessionID );
```

这说明第二层逻辑是：

```text
35=D
-> 调 onMessage(const NewOrderSingle&, ...)
```

### 12.4 这一段代码是谁生成的

它不是手写的。

`MessageCracker.xsl` 里有非常直接的模板：

```xsl
<xsl:for-each select="//fix/messages/message">
if( msgTypeValue == "<xsl:value-of select="@msgtype"/>" )
  return onMessage( (const <xsl:value-of select="@name"/>&)message, sessionID );
</xsl:for-each>
```

这意味着：

- XML 里每出现一条 message
- XSLT 就自动生成一条 `if msgtype == ...`

所以：

- `D -> NewOrderSingle`
- `8 -> ExecutionReport`

这种映射根本不是手写分支，而是直接从 XML message 列表批量生成出来的。

### 12.5 为什么它可以直接把 `message` cast 成 `NewOrderSingle`

这是这一阶段最容易让人“半懂不懂”的一点。

关键在于：

1. `NewOrderSingle` 继承 `FIX42::Message`
2. `FIX42::Message` 继承 `FIX::Message`
3. 这些生成出来的强类型消息类几乎不新增数据成员
4. 它们主要只是增加：
   - `MsgType()`
   - 构造函数
   - `FIELD_SET` 访问方法

也就是说：

- 底层真实承载字段数据的，还是基类 `Message / FieldMap`
- `NewOrderSingle` 更像这个底层消息的“强类型视图”

所以 QuickFIX 的做法其实非常“工程化”：

- 不重新拷贝一份消息
- 直接把同一份底层字段存储，用更具体的消息类接口来看待

### 12.6 这就解释了“35=D 自动变成 FIX42::NewOrderSingle”的真正含义

它真正发生的不是：

- 运行时神奇地 new 了一个完全独立的 `NewOrderSingle`

而是：

1. 这条消息已经被解析成一个 FIX4.2 语义下的消息容器
2. 分发器读取 `35=MsgType`
3. 如果是 `"D"`，就把它按 `NewOrderSingle` 这个接口类型来调用

所以更准确地说：

> `35=D` 会让同一份底层消息数据，被当作 `FIX42::NewOrderSingle` 这类强类型接口来处理。

这也是为什么你写业务时，能直接收到：

```cpp
void onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&);
```

而不需要自己手动判断 `35`。

---

## 13. 把整个链路连成一条运行时故事

现在我们把这阶段的所有拼图串成一个完整故事。

### 13.1 发送侧：你自己构造 `FIX42::NewOrderSingle`

例如在 `tradeclient` 里：

1. 代码创建 `FIX42::NewOrderSingle`
2. 这个类自动带：
   - `BeginString=FIX.4.2`
   - `MsgType=D`
3. 必填字段通过构造函数设置
4. 可选字段通过 `set(...)` 设置
5. Header 也继续补字段
6. 最终 `Message::toString()` 变成 FIX 字符串发出去

### 13.2 协议层：这类消息类的结构来自 XML

因为：

- `NewOrderSingle.h` 是从 `FIX42.xml` 生成的
- 哪些字段可用、哪些必填、有哪些 group，也都是 XML 决定的

### 13.3 接收侧：先还原成通用消息容器

收到一条 FIX 字符串后，底层先解析成消息对象。

这时它的核心信息已经在 header/body/trailer 里了：

- `8=FIX.4.2`
- `35=D`
- `11=...`
- `55=...`
- ...

### 13.4 版本分发：看 `BeginString`

公共 `FIX::MessageCracker` 看见：

- `FIX.4.2`

于是把它交给：

- `FIX42::MessageCracker`

### 13.5 类型分发：看 `35`

`FIX42::MessageCracker` 看见：

- `35=D`

于是执行：

```cpp
onMessage((const NewOrderSingle&)message, sessionID);
```

### 13.6 业务层：你收到强类型参数

最终你的应用代码收到：

```cpp
void onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&);
```

于是你就可以直接：

- `message.get(clOrdID);`
- `message.get(symbol);`
- `message.get(side);`
- `message.get(ordType);`

而不用自己手动解释 `35=D` 或者逐个 tag 做 if/else。

---

## 14. 这一阶段最容易误解的几个点

### 14.1 误解一：`FIX42::NewOrderSingle` 是完全独立的数据模型

不完全对。

更准确地说：

- 它是建立在通用 `Message / FieldMap` 之上的强类型包装层

### 14.2 误解二：收到消息后，QuickFIX 会重新“解析成专门的 NewOrderSingle 对象”

更准确地说：

- QuickFIX 底层先有通用消息数据
- `MessageCracker` 再按类型把它交给强类型接口

### 14.3 误解三：`MessageCracker` 逻辑是手写维护的

不对。

至少在版本化 `fix42/MessageCracker.h` 这一层：

- 它是从 XML 批量生成的

### 14.4 误解四：强类型消息类的构造函数是人工设计的

不对。

它非常明显是：

- 由 XML 里 required 字段列表生成出来的

---

## 15. 如果你现在回头再看这几个文件，应该带着什么问题看

这一阶段最适合的不是“从头到尾死读”，而是带问题看。

### 15.1 再看 `FieldMap.h`

重点盯：

- 字段怎么存
- group 怎么存
- `FIELD_SET` 宏是什么

### 15.2 再看 `Message.h`

重点盯：

- Header / Body / Trailer 的三段结构
- `toString()` / `setString()` 的角色
- `Message(const BeginString&, const MsgType&)`

### 15.3 再看 `fix42/Message.h`

重点盯：

- `BeginString("FIX.4.2")` 是怎么固定的
- 为什么这是版本化消息的基类

### 15.4 再看 `fix42/NewOrderSingle.h`

重点盯：

- `MsgType()` 为什么是 `"D"`
- 构造函数为什么正好是那 6 个参数
- `FIELD_SET` 为什么铺满全文件
- `NoAllocs` 为什么会变成内嵌类

### 15.5 再看 `fix42/ExecutionReport.h`

重点盯：

- 必填构造函数和 XML required 字段的对应关系
- 为什么示例程序会构造后继续 `set(...)`

### 15.6 再看 `fix42/MessageCracker.h`

重点盯：

- `35=D` 到 `NewOrderSingle`
- `35=8` 到 `ExecutionReport`
- 默认 `UnsupportedMessageType`

### 15.7 再看 `FIX42.xml`

重点盯：

- `message name`
- `msgtype`
- `required='Y'`
- `group`

只要这四类信息一眼能对应到生成代码，你这阶段就算真正看懂了。

---

## 16. 第二阶段结束后，你应该能回答什么

如果这一阶段已经吃透，你现在应该能比较顺地回答这些问题：

1. `Message` 和 `FieldMap` 是什么关系？
2. 为什么 `Message` 可以同时表示出站消息和入站消息？
3. `FIX42::Message` 比通用 `Message` 多了什么？
4. `FIX42::NewOrderSingle` 的必填构造函数为什么是那 6 个参数？
5. `FIX42::ExecutionReport` 的必填构造函数为什么是那 10 个参数？
6. `NoAllocs`、`NoContraBrokers` 为什么会变成内嵌 group 类？
7. `35=D` 为什么会自动落到 `onMessage(const FIX42::NewOrderSingle&, ...)`？
8. 为什么这种“自动变成强类型”其实更像一种强类型视图，而不是重新造对象？

如果这些问题你已经基本能自己讲出来，第二阶段就算真正过关了。

---

## 17. 这一阶段压缩成一句话

最后把这一阶段压缩成一句最关键的话：

> `spec/FIX42.xml` 定义了消息结构；生成器把这些定义变成 `FIX42::NewOrderSingle`、`FIX42::ExecutionReport` 等强类型消息类，以及 `FIX42::MessageCracker` 里的类型分发代码；运行时底层仍然是通用 `Message / FieldMap`，只是根据 `BeginString` 和 `35=MsgType` 被当作对应的强类型接口来处理。

---

## 18. 下一步最自然的衔接

第二阶段结束后，最自然的下一步就是继续往下走到：

- 会话层
- 校验层
- Session / DataDictionary / SessionSettings

也就是去看：

- 消息对象已经有了
- 那 QuickFIX 是怎么把字符串解析、校验、推进会话状态、再交给应用层的

换句话说：

- 第一阶段学“怎么用”
- 第二阶段学“消息对象长什么样”
- 第三阶段就该学“引擎怎么驱动这些消息对象流动起来”

