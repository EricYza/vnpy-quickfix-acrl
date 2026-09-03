# QuickFIX 项目源码全景导读

这份文档的目标不是只告诉你“怎么编译”，而是帮助你真正建立对 QuickFIX 的工程脑图：

1. 这个仓库里每一层代码是干什么的。
2. 一个 FIX 报文是怎样从字符串一路变成 C++ 对象，又怎样被发送到网络上的。
3. 你改某个文件时，到底会影响“头文件接口”“动态库实现”还是“示例程序行为”。

这份导读尽量详细，但有一个现实前提要先说明：

- QuickFIX 里有很多“成套自动生成”的文件，尤其是 `src/C++/fix40` 到 `src/C++/fix50sp2` 这些目录。
- 它们往往是“同一种模板，按不同 FIX 版本生成出一大批消息类”。
- 所以与其机械地把几百个文件逐个重复解释，不如把“这类文件的共同模式、生成来源、调用关系”讲清楚。这样你以后看到任何一个同类文件，都能自己读懂。

---

## 1. 先建立一个总图

如果用很通俗的话来形容，QuickFIX 这个项目可以分成 6 层：

1. `spec/`
   FIX 协议说明书。定义“有哪些消息、每种消息有哪些字段、字段顺序和规则是什么”。

2. `src/C++/fix42` 这类版本目录
   根据 `spec/*.xml` 自动生成的“强类型消息类”。比如 `NewOrderSingle.h`、`OrderCancelRequest.h`、`ExecutionReport.h`。

3. `src/C++/` 核心引擎
   真正做解析、校验、会话管理、网络通信、存储、日志、SSL 的核心实现。最终编译进 `libquickfix.so`。

4. `include/quickfix/`
   对外公开的头文件安装目录。外部程序通过这里 `#include <quickfix/...>` 来使用 QuickFIX。

5. `examples/`
   示例程序，比如 `tradeclient`、`executor`、`ordermatch`。它们不是引擎本体，而是“使用 QuickFIX 库的示例应用”。

6. `src/python3/`、`src/ruby/`
   语言绑定，让 Python/Ruby 也能调用底层 C++ QuickFIX 引擎。

你可以把它想成：

- `spec/` 是协议蓝图
- `src/C++/fix42/*.h` 是根据蓝图印出来的标准零件
- `src/C++/*.cpp` 是发动机和传动系统
- `examples/*` 是把发动机装到一辆演示车上

---

## 2. 你之前做的 clone / cmake / build / install，到底发生了什么

这部分非常重要，因为你现在已经不是只想“会用”，而是想知道自己每一步在做什么。

### 2.1 `git clone`

你执行：

```bash
git clone https://github.com/quickfix/quickfix.git QuickFIX
```

本质上只是把“源代码仓库”下载下来。

这一步拿到的是：

- `.cpp` / `.h` 源码
- `CMakeLists.txt`
- `spec/*.xml`
- `examples/`
- `test/`
- 文档和脚本

这一步**没有编译任何东西**。

### 2.2 `cmake -S . -B build ...`

你执行的这一类命令：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DQUICKFIX_SHARED_LIBS=ON \
  -DQUICKFIX_EXAMPLES=ON \
  -DHAVE_SSL=ON
```

本质是“生成构建方案”，不是编译。

它做了几件事：

1. 读取顶层 `CMakeLists.txt`
2. 识别你当前系统、编译器、依赖库
3. 确定这次要不要启用 SSL、示例程序、共享库
4. 递归读取 `src/`、`examples/` 等子目录的 `CMakeLists.txt`
5. 在 `build/` 目录下生成 Makefile、缓存文件、目标依赖关系

你可以把它理解成：

- `cmake` 不是工人
- `cmake` 更像“施工总包”，先出施工图

生成出来的典型文件包括：

- `build/CMakeCache.txt`
- `build/Makefile`
- `build/src/C++/CMakeFiles/...`
- `build/examples/...`

这些文件都是“生成物”，不是源码真身。

### 2.3 `cmake --build build`

这一步才是真正编译。

它会按照前面 CMake 生成的构建规则：

1. 编译 `src/C++/*.cpp`
2. 把它们链接成 `quickfix` 库
3. 再编译 `examples/*` 里的示例程序
4. 把可执行文件和库放到输出目录

在你这次配置下，核心产物包括：

- `lib/libquickfix.so`
- `lib/libquickfix.so.17`
- `lib/libquickfix.so.17.0.0`
- `lib/executor`
- `lib/tradeclient`
- `lib/ordermatch`

### 2.4 `cmake --install build`

这一步是“安装产物到系统目录”。

在你的配置里安装到了 `/usr/local`，所以会把内容复制到：

- `/usr/local/include/quickfix/`
- `/usr/local/lib/`
- `/usr/local/share/quickfix/`
- `/usr/local/share/cmake/quickfix/`

也就是说：

- 头文件被安装到系统 include 目录
- 动态库被安装到系统 lib 目录
- FIX XML 规范文件被安装到 share 目录

### 2.5 `ldconfig`

`sudo ldconfig` 的作用是刷新 Linux 动态链接器缓存。

因为你刚把 `libquickfix.so` 安装到了 `/usr/local/lib`，系统需要重新扫描动态库索引，这样别的程序运行时才能顺利找到它。

你看到的这个提示：

```text
/sbin/ldconfig.real: /usr/lib/wsl/lib/libcuda.so.1 is not a symbolic link
```

和 QuickFIX 本身没关系，是 WSL 环境里 CUDA 相关库的提示，通常可以忽略。

---

## 3. 顶层目录逐个讲

下面开始讲整个仓库每个主要目录在工程中的角色。

### 3.1 `CMakeLists.txt`

这是顶层 CMake 入口，相当于整个项目的“总装配说明书”。

它负责：

- 定义项目名和版本
- 指定 C++17
- 定义可选功能开关
- 查找 OpenSSL / MySQL / PostgreSQL / ODBC / Python3
- 决定是否构建示例和测试
- 把构建继续分发给 `src/` 和 `examples/`

重点信息：

- `project(... VERSION 1.16.0 ...)`
  这是项目版本号。
- `set(CMAKE_CXX_STANDARD 17)`
  表示要求 C++17。
- `option(HAVE_SSL ...)`
  控制是否启用 SSL 相关源码。
- `option(QUICKFIX_SHARED_LIBS ...)`
  控制编译动态库还是静态库。
- `add_subdirectory(src)`
  进入核心源码目录。
- `add_subdirectory(examples)`
  进入示例程序目录。

一句话概括：

`CMakeLists.txt` 决定“这次要造什么”和“要不要把某些模块装上去”。

### 3.2 `configure.ac`、`Makefile.am`、`m4/`

这是旧的 Autotools 构建系统。

QuickFIX 这个项目同时支持两套构建方法：

1. 新一些的 `CMake`
2. 较传统的 `Autotools`

所以你会同时看到：

- `CMakeLists.txt`
- `configure.ac`
- `Makefile.am`
- `bootstrap`
- `m4/`

如果你用的是：

```bash
./bootstrap
./configure
make
make install
```

走的就是这条旧链路。

你现在实际用的是 CMake，所以 Autotools 文件暂时不需要深入，但它们说明这个项目历史比较久、兼容面比较广。

### 3.3 `README.md`、`README.SSL`、`NEWS`、`LICENSE`、`SECURITY.md`

这些属于项目说明层：

- `README.md`
  总入口说明
- `README.SSL`
  SSL 相关说明
- `NEWS`
  版本变更历史
- `LICENSE`
  许可证
- `SECURITY.md`
  安全披露相关

它们不参与编译，但帮助你理解项目定位和历史。

### 3.4 `bin/`

这个目录最容易让初学者困惑。

在 Linux/WSL 构建时，`bin/` 里的 `executor`、`tradeclient`、`ordermatch` 往往不是“真正的二进制本体”，而是指向真实可执行文件的符号链接。

真实文件通常在：

- `lib/executor`
- `lib/tradeclient`
- `lib/ordermatch`

之所以这样做，是为了让你在仓库里更方便地运行示例程序。

同时 `bin/cfg/` 里也放了示例配置文件和 SSL 证书目录。

### 3.5 `lib/`

这是构建输出目录。

在这个项目当前的 CMake 里，库和可执行文件默认都输出到顶层 `lib/`。

这里通常放：

- `libquickfix.so*`
- 示例程序真实二进制
- 某些测试二进制

所以 `lib/` 不是“源代码目录”，而是“编译结果目录”。

### 3.6 `build/`

这是 CMake 生成的构建目录。

它的重要性在于：

- 它保存了这次构建的配置结果
- 它知道每个目标该怎么编译
- 它记录了目标依赖、编译参数、链接参数

但它不是你应该手改的地方。

你看到的这些文件：

- `build/examples/.../Makefile`
- `build/.../cmake_install.cmake`
- `build/CMakeFiles/...`

都属于“派生文件”。

### 3.7 `doc/`

这是文档目录。

里面主要是：

- `Doxyfile`
- `document.sh`
- `document.bat`
- `html/`

也就是说 QuickFIX 文档主要通过 Doxygen 生成。

你现在看的这份导读也放在这里，是为了把“外部用户文档”和“源码导读文档”放在一起。

### 3.8 `scripts/`

这里通常是环境或构建辅助脚本。

不是核心逻辑，但在某些平台上会用到。

### 3.9 `test/`

这是测试运行脚本、测试配置和辅助 Ruby 脚本的目录。

它和 `src/C++/test/` 的关系是：

- `src/C++/test/` 更偏“测试源码”
- `test/` 更偏“测试运行框架、配置、脚本、夹具”

这个区分很重要。

---

## 4. `src/` 目录是整个工程的核心区

顶层 `src/` 下面主要有几块：

- `src/C++`
- `src/python`
- `src/python3`
- `src/ruby`
- `src/sql`
- `src/swig`

另外还有几个单独文件：

- `src/at.cpp`
- `src/pt.cpp`
- `src/ut.cpp`
- `src/quickfix.i`

下面分别讲。

---

## 5. `src/C++/` 是 QuickFIX 引擎本体

这是最核心的目录。

你可以把它分成三类内容：

1. 核心引擎 `.cpp/.h`
2. 各个 FIX 版本的自动生成消息头文件
3. 测试源码

### 5.1 真正被编进 `libquickfix.so` 的，是哪些文件

不是 `src/C++/` 下所有东西都会进动态库。

真正进核心库的是 `src/C++/CMakeLists.txt` 里 `quickfix_SOURCES` 列出来的那些 `.cpp` 文件，比如：

- `Session.cpp`
- `Message.cpp`
- `FieldMap.cpp`
- `DataDictionary.cpp`
- `SocketAcceptor.cpp`
- `SocketInitiator.cpp`
- `FileStore.cpp`
- `FileLog.cpp`
- `Utility.cpp`
- `UtilitySSL.cpp`（启用 SSL 时）

这些 `.cpp` 会先编译成目标文件，再链接成 `libquickfix.so`。

而 `.h` 头文件通常不会“单独编译成一个东西”，而是：

- 在编译某个 `.cpp` 时被 `#include`
- 参与模板、内联函数、类声明
- 最后以“安装头文件”的形式暴露给外部程序

这就是为什么：

- 改 `.cpp` 往往会影响动态库本身
- 改某些 `.h` 可能不一定改库二进制，但会影响依赖这些头文件重新编译的程序

### 5.2 `include/quickfix/` 和 `src/C++/` 的关系

这两个目录看起来像两个地方都在放头文件，容易混淆。

可以这样理解：

- `src/C++/` 是源码编辑区
- 安装时，`src/C++/*.h` 会被复制到 `/usr/local/include/quickfix/`
- 构建时，对外暴露的 include 接口路径被设置成 `quickfix/...`

所以你在示例程序里看到：

```cpp
#include "quickfix/Session.h"
#include "quickfix/fix42/ExecutionReport.h"
```

逻辑上就是在使用 QuickFIX 的公开接口。

### 5.3 这一层最重要的几个“基础文件族”

下面开始讲核心 C++ 文件的分工。你不用一开始就逐个背下来，但最好先知道它们属于哪一层。

#### A. 字段层

这些文件处理“FIX 字段”本身：

- `Field.h`
- `FieldTypes.h`
- `Fields.h`
- `FixFields.h`
- `FixCommonFields.h`
- `FieldNumbers.h`
- `FixFieldNumbers.h`
- `Values.h`
- `FieldConvertors.h/.cpp`
- `FieldTypes.cpp`

通俗说，这一层解决的是：

- tag 55 是什么字段
- `Price`、`ClOrdID`、`Side` 在 C++ 里分别怎么表示
- 字符串 `"123.45"` 怎么转成价格对象
- 字段值怎么从对象转回 FIX 字符串

这是“原子零件层”。

#### B. 容器层

这些文件把字段组织成“消息结构”：

- `FieldMap.h/.cpp`
- `Group.h/.cpp`
- `Message.h/.cpp`
- `MessageSorters.h/.cpp`

你可以这样理解：

- `FieldMap` 是“字段容器”
- `Group` 是“重复组容器”
- `Message` 是“完整报文对象”

其中 `Message.h` 很关键，因为它定义了：

- `Header`
- `Message`
- `Trailer`

也就是一个 FIX 报文在内存里的三段结构。

#### C. 协议校验层

这些文件负责“这个报文合不合协议规范”：

- `DataDictionary.h/.cpp`
- `DataDictionaryProvider.h/.cpp`
- `Parser.h/.cpp`

它们依赖 `spec/*.xml` 生成/安装出来的 FIX 规范来判断：

- 这个 `MsgType` 合不合法
- 某个 tag 是否允许出现在这个消息里
- 某个字段是否必填
- 某个枚举值是否超出范围

你之前碰到的：

- `Value is incorrect (out of range) for this tag`

就是这一层在起作用。

#### D. 会话层

这是 QuickFIX 最核心的“引擎逻辑层”：

- `SessionID.h`
- `SessionState.h`
- `Session.h/.cpp`
- `SessionFactory.h/.cpp`
- `SessionSettings.h/.cpp`
- `Settings.h/.cpp`
- `Initiator.h/.cpp`
- `Acceptor.h/.cpp`
- `TimeRange.h/.cpp`

这一层解决的是：

- 谁和谁建会话
- SenderCompID / TargetCompID 如何组成一个会话
- 序号该从多少开始
- 心跳多久发一次
- 对端断线后如何重连
- Logon / Logout / ResendRequest / SequenceReset 如何处理

如果把 FIX 引擎比作一个交易柜台，这一层就是“柜台经理”，负责整个会话生命周期。

`Session.cpp` 是整个项目最值得反复读的核心文件之一。

#### E. 网络传输层

这层把“会话逻辑”接到真正的 socket 上：

- `SocketAcceptor.h/.cpp`
- `SocketInitiator.h/.cpp`
- `SocketConnection.h/.cpp`
- `SocketConnector.h/.cpp`
- `SocketServer.h/.cpp`
- `SocketMonitor_UNIX.cpp`
- `SocketMonitor_WIN32.cpp`
- `ThreadedSocketAcceptor.cpp`
- `ThreadedSocketInitiator.cpp`
- `ThreadedSocketConnection.cpp`

含义大概是：

- `Acceptor`
  等别人连我
- `Initiator`
  我主动连别人
- `SocketConnection`
  单条连接对象
- `SocketServer`
  监听 socket
- `SocketMonitor_*`
  平台相关的 socket 监视实现

这层并不关心“订单业务”，它只关心：

- 连接建立了没有
- 某条连接收到了什么字符串
- 该把字符串交给谁继续处理

#### F. SSL 传输层

启用 `HAVE_SSL=ON` 后会额外编译：

- `SSLSocketAcceptor.cpp`
- `SSLSocketInitiator.cpp`
- `SSLSocketConnection.cpp`
- `ThreadedSSLSocketAcceptor.cpp`
- `ThreadedSSLSocketInitiator.cpp`
- `ThreadedSSLSocketConnection.cpp`
- `UtilitySSL.cpp`

它们是普通 socket 传输层的 SSL 版本。

你之前看到的 OpenSSL 3.0 deprecated warning，主要就来自这部分。

这些 warning 的意思是：

- QuickFIX 的 SSL 实现还在调用一些 OpenSSL 老接口
- 但当前 OpenSSL 版本把它们标成“已弃用”
- 它通常不妨碍编译和运行
- 只是说明这块代码偏老，需要以后升级适配

#### G. 存储和日志层

这些文件负责消息持久化和会话日志：

- `MessageStore.h/.cpp`
- `FileStore.h/.cpp`
- `NullStore.h/.cpp`
- `FileLog.h/.cpp`
- `Log.h/.cpp`

以及数据库版本：

- `MySQLStore.cpp`
- `MySQLLog.cpp`
- `PostgreSQLStore.cpp`
- `PostgreSQLLog.cpp`
- `OdbcStore.cpp`
- `OdbcLog.cpp`

这层解决的是：

- 发出去的消息要不要落盘
- 收到的消息要不要记录
- 下次重启后序号如何恢复
- 重传时历史消息从哪取出来

如果不用数据库，一般最常见的就是 `FileStore` + `FileLog`。

#### H. 杂项基础设施层

包括：

- `Dictionary.*`
- `Utility.*`
- `HostDetailsProvider.*`
- `HttpConnection.*`
- `HttpMessage.*`
- `HttpParser.*`
- `HttpServer.*`
- `PUGIXML_DOMDocument.*`
- `pugixml.cpp`
- `stdafx.*`

这里面有些是基础工具，有些是辅助功能。

例如：

- `Dictionary`
  更偏通用键值配置结构
- `Utility`
  字符串、时间、杂项工具
- `pugixml`
  XML 处理依赖实现

---

## 6. `src/C++/fix40` 到 `fix50sp2`：自动生成的“强类型消息世界”

这是整个项目第二个最重要的区域。

目录包括：

- `src/C++/fix40`
- `src/C++/fix41`
- `src/C++/fix42`
- `src/C++/fix43`
- `src/C++/fix44`
- `src/C++/fix50`
- `src/C++/fix50sp1`
- `src/C++/fix50sp2`
- `src/C++/fixt11`

这些目录里通常会看到：

- `Message.h`
- `MessageCracker.h`
- 一大堆消息类头文件
  例如 `NewOrderSingle.h`、`ExecutionReport.h`、`OrderCancelRequest.h`

### 6.1 它们不是手写业务代码，而是协议代码生成结果

这些文件的源头不是“开发者一行一行手写”，而是：

1. `spec/FIX42.xml` 这类 XML 规范
2. `spec/GeneratorCPP.rb`
3. `spec/generate_c++.sh`

也就是说：

- `spec/*.xml` 描述协议
- 生成器脚本读取 XML
- 自动产出各版本 C++ 消息头文件

### 6.2 这类文件的共同模式

以 `src/C++/fix42/OrderCancelRequest.h` 为例，它做的事情其实很标准：

1. 定义一个 `FIX42::OrderCancelRequest` 类
2. 让它继承 `FIX42::Message`
3. 静态声明自己的 `MsgType()` 是 `"F"`
4. 给出一个带必填字段的构造函数
5. 通过很多 `FIELD_SET(...)` 宏，生成字段访问接口

所以这个类本质上是：

- “FIX 4.2 版本下，消息类型 F 的一个强类型包装器”

它不是传输层，不管 socket，也不管心跳。
它只是让你不用手写：

```text
35=F
41=...
11=...
55=...
54=...
```

而是能用 C++ 对象来构造和读取这些字段。

### 6.3 `MessageCracker.h` 是干什么的

这个文件非常关键，也最值得初学者理解。

它本质是“消息分发器”。

以 `src/C++/fix42/MessageCracker.h` 为例：

- 它为很多 FIX42 消息定义了 `onMessage(...)` 重载
- 默认实现通常是抛 `UnsupportedMessageType`
- 当你调用 `crack(message, sessionID)` 时
- 它会根据消息头里的 `35=MsgType`
- 自动把通用 `Message` 分发到正确的强类型处理函数

通俗说：

- 没有 `MessageCracker`，你得自己 `if (msgType == "D") ...`
- 有了它，你只要重载：

```cpp
void onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&);
```

引擎就会自动帮你把 `35=D` 的消息派发过来。

这就是你在 `examples/executor/C++/Application.cpp` 里看到的核心模式。

### 6.4 改这些生成头文件意味着什么

这里要特别小心。

如果你直接改了例如：

- `src/C++/fix42/MessageCracker.h`
- `src/C++/fix42/OrderCancelRequest.h`

那你改的是“协议生成结果”。

后果是：

1. 依赖这些头文件的程序需要重新编译
2. 如果你之后重新跑生成脚本，手改内容可能被覆盖

所以更稳妥的思路通常是：

- 如果是协议定义变化，改 `spec/*.xml` 和生成器流程
- 如果只是应用处理逻辑变化，改 `examples/*` 或你自己的应用代码

---

## 7. `spec/`：协议蓝图和代码生成器

这个目录是 QuickFIX 的“协议元数据源头”。

里面最重要的是：

- `FIX40.xml`
- `FIX41.xml`
- `FIX42.xml`
- `FIX43.xml`
- `FIX44.xml`
- `FIX50.xml`
- `FIX50SP1.xml`
- `FIX50SP2.xml`
- `FIXT11.xml`

这些 XML 文件定义了：

- 消息类型
- 字段定义
- 枚举值
- 必填/可选
- 组件和重复组结构

### 7.1 为什么它这么重要

因为 QuickFIX 不只是“拿字符串转对象”。

它很多能力都来自这里：

- 生成版本化消息头文件
- 校验消息结构
- 校验字段合法性
- 输出更强类型的 API

### 7.2 生成器相关文件

`spec/` 里还有：

- `Generator.rb`
- `GeneratorCPP.rb`
- `GeneratorPython.rb`
- `GeneratorRuby.rb`
- `generate.sh`
- `generate_c++.sh`
- `generate_spec.rb`
- `MessageCracker.xsl`
- `Values.xsl`

这一组文件负责把 FIX 规范转成代码。

可以这样理解：

- XML 是“协议数据库”
- 生成器是“代码工厂”
- `src/C++/fix42/*.h` 是“工厂产出的零件”

---

## 8. `examples/`：这不是引擎本身，而是“怎么使用引擎”的示范

目录有：

- `examples/executor`
- `examples/tradeclient`
- `examples/ordermatch`

这三个程序非常重要，因为它们是理解 QuickFIX 的最佳入口。

### 8.1 `examples/tradeclient`

这是一个交互式客户端示例。

它的角色是：

- 读取配置
- 建一个 FIX initiator
- 连到对端
- 通过终端菜单让你发单、撤单、改单、做行情请求

你之前实际用到的就是它。

关键文件通常是：

- `Application.cpp`
- `Application.h`
- `tradeclient.cpp`
- `CMakeLists.txt`

其中：

- `tradeclient.cpp`
  负责程序启动、读取配置、创建 initiator
- `Application.cpp`
  负责应用层回调和交互菜单逻辑

### 8.2 `examples/executor`

这是一个非常简单的服务端示例。

它的角色是：

- 接收客户端连接
- 处理登录
- 接收订单
- 构造成交回报

但它很简化，不是完整交易所。

你之前测试时已经验证到一个关键事实：

- 它只支持 `NewOrderSingle`
- 只接受 limit 单
- 不支持成功处理 `OrderCancelReplaceRequest`

这正是因为它的 `Application.cpp` 只重载了若干 `onMessage(const FIXxx::NewOrderSingle&, ...)` 处理函数。

### 8.3 `examples/ordermatch`

这个比 `executor` 更接近一个小型撮合示例。

它额外支持：

- `OrderCancelRequest`
- `MarketDataRequest`
- 简单订单簿撮合

所以如果你想看：

- 撤单消息怎么处理
- 行情请求怎么处理
- 订单在内存里怎么匹配

`ordermatch` 比 `executor` 更值得读。

### 8.4 示例程序是如何和核心库连接起来的

以 `examples/executor/C++/CMakeLists.txt` 为例：

- `add_executable(...)`
  创建可执行文件
- `target_link_libraries(... ${PROJECT_NAME})`
  把它链接到核心 QuickFIX 库

也就是说：

- `executor.cpp` 不是“动态库里的源码直接运行”
- 而是编译成一个单独可执行文件
- 运行时再加载并调用 `libquickfix.so`

这是一个非常关键的认识。

---

## 9. `bin/cfg/`：示例配置和证书

你之前实际操作时大量接触了这个目录。

这里通常包含：

- `executor.cfg`
- `tradeclient.cfg`
- 本地测试用的 `*_local.cfg`
- SSL 版本配置
- `certs/`

这些文件不是代码，但它们直接影响运行行为。

### 9.1 配置文件控制什么

QuickFIX 配置文件会控制：

- BeginString
- SenderCompID / TargetCompID
- 监听端口或连接地址
- 心跳时间
- 日志目录
- store 目录
- 是否用 SSL
- SSL 证书路径
- 是否允许哪些远端地址

你之前遇到的：

- 登录成功后立刻断开
- `Deny connections to the acceptor from 127.0.0.1`

就是配置限制导致的，不是库编译坏了。

### 9.2 `certs/` 为什么会出问题

QuickFIX 自带示例证书往往比较老。

你之前遇到：

- `certificate has expired`

不是代码错，而是示例证书过期了。

所以你后来重新生成：

- CA
- server cert
- client cert

这一步是 SSL 配置层的问题，不是 C++ 编译层的问题。

---

## 10. `src/python3/`、`src/python/`、`src/ruby/`：语言绑定层

这部分说明 QuickFIX 并不只给 C++ 用。

### 10.1 `src/quickfix.i`

这是 SWIG 接口文件，非常关键。

它定义了：

- 哪些 C++ 类要暴露给脚本语言
- 名字怎么映射
- 某些异常怎么翻译成 Python/Ruby 异常
- 某些类在无 SSL/MySQL/PostgreSQL 时用 stub 代替

可以把它理解成：

- C++ 世界和脚本语言世界之间的翻译合同

### 10.2 `src/python3/`

这里主要包括：

- `QuickfixPython.cpp`
- `QuickfixPython.h`
- `quickfix.py`
- `quickfix42.py` 等版本模块
- `CMakeLists.txt`

它们的关系是：

- `_quickfix` 这个底层扩展模块由 C++ 编译出来
- `quickfix.py`、`quickfix42.py` 这类 Python 文件提供上层封装

所以 Python 版 QuickFIX 不是纯 Python 实现，而是：

- C++ 引擎 + Python 包装层

### 10.3 `src/ruby/`

同理，Ruby 目录也是：

- 底层 C++ 绑定
- 上层 Ruby 封装

### 10.4 `src/swig/`

这个目录里放的是一些 stub 头文件，例如：

- `SSLStubs.h`
- `MySQLStubs.h`
- `PostgreSQLStubs.h`

它们的用途是：

- 当某些可选能力没打开时
- 给 SWIG 绑定提供替代声明
- 让绑定层不至于因为缺少这些类而编不过

---

## 11. `src/sql/`：数据库存储后端的建表脚本

这个目录一般按数据库类型分子目录，例如：

- `mysql`
- `postgresql`
- `mssql`

它们不是 C++ 核心逻辑，而是给数据库版 store/log 用的 schema 脚本。

因为 QuickFIX 支持：

- 用文件保存会话状态
- 也支持用数据库保存会话状态和日志

所以这里放的是数据库初始化所需 SQL。

---

## 12. `src/ut.cpp`、`src/at.cpp`、`src/pt.cpp`：测试和性能入口

这几个文件很值得单独提。

### 12.1 `ut.cpp`

它是单元测试入口。

真正的测试用例大多在：

- `src/C++/test/*.cpp`

例如：

- `SessionTestCase.cpp`
- `DataDictionaryTestCase.cpp`
- `FileStoreTestCase.cpp`

而 `ut.cpp` 更像“测试主函数”。

### 12.2 `pt.cpp`

它是性能测试入口。

### 12.3 `at.cpp`

它是 acceptance test 入口，更偏端到端行为测试。

所以：

- `src/ut.cpp` / `pt.cpp` / `at.cpp` 是入口
- `src/C++/test/` 是大量单元测试实现
- `test/` 目录是脚本、配置和测试运行支持

---

## 13. `src/C++/test/`：核心库级别测试源码

这个目录里放的是真正的 C++ 测试用例。

常见命名模式是：

- `XxxTestCase.cpp`

比如：

- `SessionTestCase.cpp`
- `ParserTestCase.cpp`
- `UtilitySSLTestCase.cpp`
- `SocketAcceptorTestCase.cpp`

这说明测试组织方式比较直接：

- 一个核心类/模块
- 配一个测试文件

如果你未来想改核心库，先搜对应的 `*TestCase.cpp` 是非常高效的做法。

---

## 14. 你现在最关心的几个文件，放在整个工程里分别是什么角色

因为你最近一直在看这几个文件，我把它们直接嵌回总图里。

### 14.1 `examples/executor/C++/executor.cpp`

这是示例程序入口，不是核心库源码。

它的职责通常是：

- 读取命令行参数
- 载入 session settings
- 创建 store factory / log factory / application
- 根据是否带 `SSL` 参数选择：
  - `SocketAcceptor`
  - 或 `ThreadedSSLSocketAcceptor`
- 启动 acceptor

所以它更像：

- “把 QuickFIX 库组装起来并跑起来”的 main 函数

### 14.2 `examples/executor/C++/Application.cpp`

这是 executor 的应用逻辑。

它继承/组合了 QuickFIX 提供的应用接口，并通过 `crack(...)` 分发消息。

它只处理少量业务消息，因此功能很有限。

你之前测试出来：

- limit 单可以
- replace 不支持

原因就在这里，不是 QuickFIX 引擎整体不支持，而是“这个示例应用没实现那条业务逻辑”。

### 14.3 `src/C++/fix42/MessageCracker.h`

这是 FIX42 版本的消息分发器头文件。

它属于“自动生成的协议层”，不是网络层、不是会话层。

### 14.4 `src/C++/fix42/OrderCancelRequest.h`

这是 FIX42 版本 `35=F` 撤单消息的强类型封装。

它属于“自动生成消息类”，不是业务逻辑实现。

### 14.5 `src/C++/Message.cpp` / `Message.h`

这是通用消息容器实现。

它属于整个引擎最底层的共用设施之一。

所有 FIX40/FIX42/FIX44 这些版本消息类，最终都建立在它之上。

### 14.6 `src/C++/Session.cpp`

这是会话引擎核心。

很多真正的 FIX 协议行为都在这里，例如：

- 登录状态推进
- 心跳处理
- 序号检查
- 重传逻辑
- 会话状态切换

如果你将来真的想深入到“QuickFIX 到底怎么工作”，这会是必读文件之一。

---

## 15. 一个报文在 QuickFIX 里走的完整路径

这一段是理解整个项目最重要的“动态流程图”。

以你发一笔 FIX42 `NewOrderSingle` 为例：

### 15.1 发送侧

1. 你在 `tradeclient` 菜单里选择 Enter Order
2. `examples/tradeclient/Application.cpp` 收集用户输入
3. 构造一个 `FIX42::NewOrderSingle` 对象
4. 这个对象继承自 `FIX42::Message`，再继承自通用 `FIX::Message`
5. `Session::sendToTarget(...)` 接手发送
6. `Session` 填充 header、seqnum、sending time
7. `Message::toString()` 把对象编码成 FIX 字符串
8. `SocketInitiator` / `SocketConnection` 把字符串发到网络上

### 15.2 接收侧

1. `SocketAcceptor` 监听到对端数据
2. `SocketConnection` 收到原始字符串
3. `Parser` / `Message` 把字符串解析成报文对象
4. `DataDictionary` 校验字段和消息结构
5. `Session` 检查会话级规则
   - 是否登录
   - seqnum 是否正确
   - 心跳/时间戳是否合理
6. 合法后，调用你的 `Application::fromApp(...)`
7. 你的 `fromApp(...)` 里通常调用 `crack(message, sessionID)`
8. `MessageCracker` 根据 `35=D` 分发到：

```cpp
onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&)
```

9. 应用逻辑处理完成后，构造 `ExecutionReport`
10. 再通过 `Session::sendToTarget(...)` 发回客户端

这就是 QuickFIX 的核心价值：

- 你不必手写底层协议收发和校验
- 你主要写 `onMessage(...)` 这一层的业务处理

---

## 16. 动态库、头文件、示例程序三者之间的关系

这一段专门回答你之前一直在追问的点。

### 16.1 什么进了动态库

主要是 `src/C++/CMakeLists.txt` 中列出的 `.cpp`。

例如：

- `Session.cpp`
- `Message.cpp`
- `SocketInitiator.cpp`
- `FileStore.cpp`

这些进 `libquickfix.so`。

### 16.2 什么没有“独立变成动态库代码块”

像这些头文件：

- `src/C++/fix42/OrderCancelRequest.h`
- `src/C++/fix42/MessageCracker.h`

它们本身不是单独链接产物。

它们的作用是：

- 在编译依赖它们的 `.cpp` 时提供类定义、内联逻辑和类型接口

### 16.3 `./executor` 到底是什么

它不是“从源码直接运行”，也不是“动态库本身”。

它是一个已经编译好的可执行文件，通常链接了：

- `libquickfix.so`

所以运行关系是：

```text
源码 -> 编译 -> 可执行文件 executor
                -> 动态库 libquickfix.so

运行时：
executor 加载 libquickfix.so
```

### 16.4 改文件后要不要重编

规则可以这样记：

- 改核心 `.cpp`
  通常要重编库
- 改示例程序 `.cpp`
  至少要重编对应示例程序
- 改公共头文件
  依赖它的目标都应该重编
- 改安装到 `/usr/local/include/quickfix` 的公开头文件
  如果外部程序用系统安装版本，也最好重新安装

最稳的做法通常是：

```bash
cmake --build build
sudo cmake --install build
sudo ldconfig
```

---

## 17. 如果你准备系统学习源码，推荐阅读顺序

如果直接从 `Session.cpp` 硬啃，很容易头大。更顺的路线是：

### 第一阶段：先把“怎么用”看懂

按这个顺序读：

1. `examples/tradeclient/tradeclient.cpp`
2. `examples/tradeclient/Application.cpp`
3. `examples/executor/C++/executor.cpp`
4. `examples/executor/C++/Application.cpp`

目标是先看懂：

- 一个应用怎么启动 QuickFIX
- `Application` 回调是怎么接上的
- `crack()` 和 `onMessage()` 是怎么工作的

### 第二阶段：补消息对象和协议层

再读：

1. `src/C++/Message.h`
2. `src/C++/FieldMap.h`
3. `src/C++/fix42/MessageCracker.h`
4. `src/C++/fix42/NewOrderSingle.h`
5. `src/C++/fix42/ExecutionReport.h`
6. `spec/FIX42.xml`

目标是看懂：

- 强类型消息类是怎么从协议定义长出来的
- `35=D` 为什么能自动变成 `FIX42::NewOrderSingle`

### 第三阶段：进入会话引擎

再读：

1. `src/C++/Session.h`
2. `src/C++/Session.cpp`
3. `src/C++/SessionFactory.cpp`
4. `src/C++/SessionSettings.cpp`
5. `src/C++/DataDictionary.cpp`

目标是看懂：

- 会话状态怎么流转
- 配置文件怎么变成运行时 session
- 消息校验在什么阶段发生

### 第四阶段：进入网络和持久化

最后读：

1. `src/C++/SocketInitiator.cpp`
2. `src/C++/SocketAcceptor.cpp`
3. `src/C++/SocketConnection.cpp`
4. `src/C++/FileStore.cpp`
5. `src/C++/FileLog.cpp`
6. `src/C++/UtilitySSL.cpp`

目标是看懂：

- 字符串到底怎么进出网络
- 消息为什么能落盘
- SSL 在哪里接进去

---

## 18. 你现在可以怎样理解整个项目

如果把 QuickFIX 压缩成一句话：

> QuickFIX 是一个“把 FIX 协议的会话管理、消息解析、校验、传输、持久化都封装好”的 C++ 引擎；你真正要写的，多数时候只是应用层的 `onMessage(...)` 处理逻辑。

如果再展开一点：

- `spec/` 定义协议
- `src/C++/fix42/*.h` 把协议变成强类型消息类
- `src/C++/*.cpp` 实现引擎
- `examples/*` 演示如何使用引擎
- `src/python3/`、`src/ruby/` 把引擎暴露给别的语言
- `test/` 和 `src/C++/test/` 负责验证这一切没坏

---

## 19. 对你当前学习阶段最实用的结论

最后给你几个非常实用的判断标准，后面你读源码时会很省劲。

### 19.1 判断一个文件属于哪一层

你可以直接问自己：

- 它是在定义“字段/消息”吗
- 它是在管理“会话状态”吗
- 它是在处理“socket 传输”吗
- 它是在做“存储/日志”吗
- 它只是“示例应用”吗
- 它是不是“自动生成文件”

一旦知道它属于哪一层，就不容易迷路。

### 19.2 判断改动后需不需要重装系统库

简单记：

- 只改你自己的示例程序逻辑
  通常重编就够
- 改 QuickFIX 核心库实现
  最好重编并重新 install
- 改公共头文件
  依赖它们的目标都要重编

### 19.3 判断某个功能“是 QuickFIX 不支持，还是示例程序没实现”

这个问题非常关键。

例如你之前碰到：

- replace 返回 `Unsupported Message Type`

这不等于 QuickFIX 引擎完全不支持该消息类型。
更准确地说，是：

- FIX42 协议层里有这个消息类
- `MessageCracker` 也知道这个类型
- 但你当前运行的 `examples/executor` 没写对应业务处理逻辑

这就是“引擎能力”和“示例应用能力”的区别。

---

## 20. 后续建议

如果你要继续深挖，最适合的下一步不是再泛泛地看目录，而是做两件事：

1. 选一条完整链路，把它从头追到底
   例如 `NewOrderSingle` 从 `tradeclient` 发出，到 `executor` 收到，再回 `ExecutionReport`

2. 选一个核心文件做精读
   我最推荐从 `Session.cpp` 或 `examples/executor/C++/Application.cpp` 开始

因为到这一步，你已经不缺“目录名解释”了，你真正需要的是把“静态结构图”变成“运行时流程图”。

如果要继续写下一篇文档，最适合的主题是：

- 《一条 FIX NewOrderSingle 在 QuickFIX 里经历了什么》
- 或《从 Session.cpp 读懂 QuickFIX 会话引擎》
