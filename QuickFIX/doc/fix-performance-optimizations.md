# FIX Parser Performance Optimizations

本文档记录当前项目中为 FIX 解析性能做的优化改动。每个优化点会说明：

- 改动位置
- 原来的行为
- 现在的行为
- 开关方式
- 对性能、正确性和实验对比的影响

## 0. 优化类型定位

当前几个优化点不属于同一类，需要先区分清楚。

SIMD stream parser、`SOH10=` pattern scan 和 busy-poll 更像是对 QuickFIX 原有通用阶段的底层优化：

```text
SIMD stream parser / SOH10 pattern scan
  -> 优化 QuickFIX 原本就有的 Parser::readFixMessage() 切完整 FIX 消息阶段
  -> 不改变 FIX 消息格式
  -> 不改变 Message/Session/Application 语义
  -> raw client 和 quickfix client 接收路径都可以受益

busy-poll
  -> 优化 QuickFIX 原本就有的 socket event loop 等待阶段
  -> 不改变 FIX 消息格式
  -> 不改变解析语义
  -> raw client 和 quickfix client 场景都可以使用
```

fixed-layout parser 则是另一类：它是非常激进的特殊化优化，不是把 QuickFIX 原始通用 parser 的每一步等价加速，而是在特定消息满足私有模板约定时绕过大量通用解析工作：

```text
fixed-layout
  -> 要求自定义 tag 9001
  -> 要求固定消息长度
  -> 要求固定字段顺序和固定字段长度
  -> 要求 marker 出现在固定 offset
  -> 命中后直接按 offset 构造 FieldBase
  -> 不命中则回退 QuickFIX 普通解析
```

因此 fixed-layout 更像是在 QuickFIX 通用 parser 前面加了一个“私有协议约定的高速入口”。它适合双方都能控制消息格式的高频下单、撤单、行情快照场景；但它和 QuickFIX typed message 的正常序列化模型不完全一致，因为 typed message 序列化不保证字段顺序、字段长度和 `9001` offset 正好满足我们的固定模板。

## 1. SIMD 字符扫描优化

### 1.1 优化目标

FIX 消息本质上是由很多 `tag=value<SOH>` 字段组成的文本协议。解析时有大量重复的单字符查找：

- 查找字段结束符 `SOH`，也就是字节值 `\001`
- 在 socket 收到的连续字节流中寻找一条完整 FIX 消息的边界
- 寻找 `8=`、`9=`、`\00110=` 这些用于切分完整消息的关键位置

原始实现主要依赖 `std::string::find` 或 `std::find` 做逐字符搜索。SIMD 优化的目标不是改变协议语义，而是把这些高频的单字符扫描换成一次比较 16 个字节的 SSE2 扫描，减少循环次数。

### 1.2 新增文件

新增了一个通用的快速字符扫描模块：

```text
src/C++/detail/FastScan.h
src/C++/detail/FastScan.cpp
```

它提供四个接口：

```cpp
const char *findCharScalar(const char *begin, const char *end, char target);
const char *findCharSimd(const char *begin, const char *end, char target);
const char *findCharFast(const char *begin, const char *end, char target);
bool simdFastScanAvailable();
```

含义如下：

- `findCharScalar`：普通逐字节扫描，作为 baseline 和 fallback。
- `findCharSimd`：使用 SSE2 一次扫描 16 字节。
- `findCharFast`：根据编译开关选择 SIMD 或 scalar。
- `simdFastScanAvailable`：benchmark 和 self-test 用来输出当前二进制是否真的编进了 SIMD 快速路径。

### 1.3 SIMD 的具体算法

实现位置：

```text
src/C++/detail/FastScan.cpp
```

核心逻辑是：

1. 用 `_mm_set1_epi8(target)` 把目标字符复制成 16 个字节的 SIMD 向量。
2. 每次用 `_mm_loadu_si128` 从输入字符串中读取连续 16 字节。
3. 用 `_mm_cmpeq_epi8` 同时比较这 16 个字节是否等于目标字符。
4. 用 `_mm_movemask_epi8` 把 16 个比较结果压成 bit mask。
5. 如果 mask 不为 0，说明当前 16 字节中至少有一个匹配字符。
6. 用 `__builtin_ctz` 或 MSVC 的 `_BitScanForward` 找出最低位的 1，也就是最靠前的匹配位置。
7. 返回这个最靠前的匹配字符地址。
8. 不足 16 字节的尾部继续走 scalar 扫描。

所以如果 16 字节里有多个目标字符，当前实现会返回最靠前的那个。这和 `std::find` / `string::find` 的语义一致。

### 1.4 编译开关

在根目录 `CMakeLists.txt` 中新增了两个独立开关：

```cmake
option(QUICKFIX_SIMD_FIELD_SCAN "Enable experimental SIMD field scanning fast paths" OFF)
option(QUICKFIX_SIMD_STREAM_PARSER "Enable experimental SIMD stream parser fast paths" OFF)
option(QUICKFIX_SIMD_PATTERN_SCAN "Enable experimental SIMD fixed-pattern scanning fast paths" OFF)
```

在 `src/C++/CMakeLists.txt` 中，如果开关打开，会向 quickfix library 添加对应编译宏：

```cmake
if(QUICKFIX_SIMD_FIELD_SCAN)
    target_compile_definitions(${PROJECT_NAME} PUBLIC QUICKFIX_SIMD_FIELD_SCAN)
endif()

if(QUICKFIX_SIMD_STREAM_PARSER)
    target_compile_definitions(${PROJECT_NAME} PUBLIC QUICKFIX_SIMD_STREAM_PARSER)
endif()

if(QUICKFIX_SIMD_PATTERN_SCAN)
    target_compile_definitions(${PROJECT_NAME} PUBLIC QUICKFIX_SIMD_PATTERN_SCAN)
endif()
```

三个开关是独立的：

- `QUICKFIX_SIMD_STREAM_PARSER`：优化从网络字节流中切出完整 FIX 消息的过程。
- `QUICKFIX_SIMD_FIELD_SCAN`：优化 `Message` 内部逐字段解析时寻找字段结束符 `SOH` 的过程。
- `QUICKFIX_SIMD_PATTERN_SCAN`：优化 stream parser 中固定字符串 `SOH10=` 的查找过程。

这样可以分别测试：

- 全部关闭，作为原始 baseline。
- 只开 stream parser SIMD。
- 只开 field scan SIMD。
- 只在 stream parser 中额外打开 `SOH10=` fixed-pattern SIMD。
- 两个都开。

### 1.5 改动一：Parser::readFixMessage 的 stream parser SIMD

改动位置：

```text
src/C++/Parser.cpp
```

原始路径：

```cpp
pos = m_buffer.find("8=");
extractLength(length, pos, m_buffer);
pos = m_buffer.find("\00110=", pos - 1);
pos = m_buffer.find("\001", pos);
```

这段逻辑负责从 `m_buffer` 中切出一条完整 FIX 消息。`m_buffer` 是 socket 收到的连续字节流，里面可能包含：

- 不完整的一条 FIX 消息
- 正好一条 FIX 消息
- 多条 FIX 消息粘在一起

现在新增了一个快速路径：

```cpp
#if defined(QUICKFIX_SIMD_STREAM_PARSER)
  if (tryReadFixMessageFast(str, m_buffer)) {
    return true;
  }
#endif
```

`tryReadFixMessageFast` 的行为是：

1. 用 SIMD 找 `8=` 起始位置。
2. 用 SIMD 找 `SOH` 后面的 `9=` 字段。
3. 手动解析 `BodyLength` 的数字。
4. 根据 `BodyLength` 计算理论上的 checksum 搜索起点。
5. 找 `SOH10=`。默认路径是先用 SIMD 找 `SOH`，再检查后面是否是 `10=`；如果打开 `QUICKFIX_SIMD_PATTERN_SCAN`，则使用 SIMD 直接匹配完整的 `SOH10=` 四字节模式。
6. 找到 checksum 字段末尾的 `SOH`。
7. 成功后从 `m_buffer` 中切出完整消息，并把已消费的字节删除。

如果快速路径失败，比如消息不完整、格式异常、关键字段还没收到，会回退到原始 `readFixMessage` 逻辑继续处理。

这个优化主要影响 server/raw 压测路径，因为 raw client 会把 FIX wire string 通过 socket 发给 QuickFIX acceptor，服务端收包后需要从 socket buffer 中切出完整 FIX 消息。

注意：parse mode 直接对单条字符串构造 `Message`，不经过 `Parser::readFixMessage()`，所以不能用 parse mode 评估 `QUICKFIX_SIMD_STREAM_PARSER`。

### 1.6 改动二：Message::extractField 的 field scan SIMD

改动位置：

```text
src/C++/Message.cpp
```

原始路径在 `Message::extractField` 中通过：

```cpp
std::find(valueStart, strEnd, '\001');
```

寻找当前字段的结束符 `SOH`。

现在封装成了 `findSoh`：

```cpp
#if defined(QUICKFIX_SIMD_FIELD_SCAN)
  const char *begin = string.data() + (valueStart - string.begin());
  const char *end = string.data() + string.size();
  const char *result = detail::findCharFast(begin, end, '\001');
  return string.begin() + (result - string.data());
#else
  return std::find(valueStart, strEnd, '\001');
#endif
```

也就是说，打开 `QUICKFIX_SIMD_FIELD_SCAN` 后，每解析一个字段时，寻找字段结束符会使用 SIMD 扫描。

这个优化会影响普通 `Message` 字段解析过程，包括 parse mode 和 server mode。之前实验中观察到它在某些消息上可能反而变慢，所以保留为独立开关，不强制和 stream parser SIMD 绑定。

### 1.7 改动三：SOH10= fixed-pattern SIMD

改动位置：

```text
src/C++/detail/FastScan.h
src/C++/detail/FastScan.cpp
src/C++/Parser.cpp
```

新增接口：

```cpp
const char *findSoh10Scalar(const char *begin, const char *end);
const char *findSoh10Simd(const char *begin, const char *end);
const char *findSoh10Fast(const char *begin, const char *end);
```

这个优化只针对完整消息切分阶段的 checksum 字段定位，也就是查找：

```text
SOH 1 0 =
```

对应 wire bytes 是：

```text
\00110=
```

SIMD 做法是一次评估 16 个可能起点：

```text
p+0 == SOH
p+1 == '1'
p+2 == '0'
p+3 == '='
```

实现上会分别加载 `current`、`current + 1`、`current + 2`、`current + 3` 的 16 字节窗口，生成四个 mask，然后做按位与：

```text
mask = mask_soh & mask_1 & mask_0 & mask_equal
```

如果 mask 不为 0，就返回最低位对应的位置，也就是最靠前的 `SOH10=` 起点。

这个优化没有替换 `SOH9=` 和 `8=`，因为它们通常离消息开头很近，直接做多字节 SIMD pattern scan 的收益不一定能覆盖额外的 vector load/compare 成本。`SOH10=` 更靠近消息尾部，尤其长消息或 repeating group 多时更可能有收益。

### 1.8 为什么 field scan 可能不一定更快

SIMD 对单字符扫描是否有收益，取决于扫描长度和调用开销。

`Parser::readFixMessage()` 处理的是 socket stream 中较长的连续 buffer，SIMD 一次处理 16 字节，比较容易摊薄额外开销。

但 `Message::extractField()` 每次通常只扫描一个字段的 value，很多字段很短，例如：

```text
35=D
54=1
40=2
```

这类字段长度很短，SIMD 的向量准备、mask 生成、尾部处理等成本可能超过逐字符扫描节省的时间。因此它被拆成单独开关，方便实验判断是否值得启用。

### 1.9 正确性影响

SIMD 扫描本身只替换“寻找某个字符的位置”这件事，不改变 FIX 字段含义，不跳过校验，也不改变 `MessageCracker`、`Session`、`DataDictionary` 的业务逻辑。

语义上要求保持和原始扫描一致：

- 找到目标字符时，返回第一个匹配位置。
- 找不到目标字符时，返回 `end`。
- 不足 16 字节的尾部走 scalar fallback。
- 平台没有 SSE2 或没有打开对应编译宏时，走 scalar fallback。

`Parser::readFixMessage()` 的快速路径如果不能确认已经拿到完整消息，会返回 `false`，然后继续走原始逻辑。因此它不会因为消息暂时不完整就强行切包。

### 1.10 测试和验证

benchmark 中加入了 SIMD 自测入口：

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
```

这个测试会把 `findCharScalar`、`findCharSimd`、`findCharFast` 在多种偏移、长度、命中位置和未命中场景下做结果对比，确认 SIMD 返回的位置和 scalar 一致。

它也会测试 `SOH10=` pattern scan，把 `findSoh10Scalar`、`findSoh10Simd`、`findSoh10Fast` 的返回位置做对比。

同时也可以运行 parser 正确性测试：

```bash
./test/run_parse_benchmark.sh --self-test-parser
```

用于确认从连续 buffer 中切完整 FIX 消息的行为仍然正确。

如果要验证真实解析流程，可以跑：

```bash
./test/run_parse_benchmark.sh --self-test-correctness --messages=1000
```

这个测试比单纯计时更重视正确性，会模拟批量消息解析过程，并检查解析出的关键字段是否符合预期。

### 1.11 性能实验建议

只测试 stream parser SIMD：

```bash
cmake -S . -B build-bench-simd-stream \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=ON \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=OFF \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=OFF \
  -DQUICKFIX_BUSY_POLL=OFF

cmake --build build-bench-simd-stream --target fix_parse_benchmark_target -j 2
```

测试 stream parser SIMD + `SOH10=` fixed-pattern SIMD：

```bash
cmake -S . -B build-bench-simd-pattern \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=ON \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=ON \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=OFF \
  -DQUICKFIX_BUSY_POLL=OFF

cmake --build build-bench-simd-pattern --target fix_parse_benchmark_target -j 2
```

测试 server/raw 路径：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=1000000 --port=54381
```

测试 field scan SIMD：

```bash
cmake -S . -B build-bench-simd-field \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=OFF \
  -DQUICKFIX_SIMD_FIELD_SCAN=ON \
  -DQUICKFIX_SIMD_PATTERN_SCAN=OFF \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=OFF \
  -DQUICKFIX_BUSY_POLL=OFF

cmake --build build-bench-simd-field --target fix_parse_benchmark_target -j 2
```

测试 parse mode：

```bash
./test/run_parse_benchmark.sh --mode=parse --message=new-order-single --messages=1000000
```

注意：parse mode 不经过 stream parser SIMD，只适合评估 `QUICKFIX_SIMD_FIELD_SCAN` 对字段解析的影响。

### 1.12 当前结论

目前 SIMD 改动分成两个方向：

- `QUICKFIX_SIMD_STREAM_PARSER`：更适合用于 server/raw 网络收包后的完整 FIX 消息切分。
- `QUICKFIX_SIMD_FIELD_SCAN`：理论上能优化字段结束符扫描，但字段较短时可能收益不稳定，所以保留独立开关。
- `QUICKFIX_SIMD_PATTERN_SCAN`：进一步优化 stream parser 中 `SOH10=` 的固定四字节模式查找，适合单独和普通 stream parser SIMD 做对比。

这两个优化和 fixed-layout parser、busy-poll 网络模式相互独立。后续可以按组合实验分别比较：

- 原始 QuickFIX baseline
- SIMD stream parser
- SIMD field scan
- fixed-layout parser
- busy-poll
- 多个优化组合后的效果

## 2. Fixed-Layout Offset Parser

### 2.1 优化目标

FIX 普通解析是通用解析：它需要从左到右扫描每个字段，找到 `=` 和 `SOH`，把 tag 转成整数，再根据数据字典、消息结构和 repeating group 规则决定字段应该放到 header、body、trailer 或 group 里。

这对任意 FIX 消息都很灵活，但对最高频、格式完全可控的消息会有额外成本。

fixed-layout parser 的目标是给少数高频消息提供一条极短路径：

- 只针对预设的固定模板消息。
- 字段顺序固定。
- 字段长度固定。
- 整条消息长度固定。
- 消息带自定义 tag `9001` 作为模板标识。
- 命中模板后，直接按 offset 提取 value，不再逐字段扫描整条消息。

当前预设了三类常用消息：

- 下单：`NewOrderSingle`，`35=D`，模板标识 `9001=NOS1`
- 撤单：`OrderCancelRequest`，`35=F`，模板标识 `9001=CXL1`
- 行情快照：`MarketDataSnapshotFullRefresh`，`35=W`，模板标识 `9001=MDW1`

### 2.2 改动位置

核心实现位置：

```text
src/C++/Message.cpp
```

benchmark 生成固定模板消息的位置：

```text
src/fix_parse_benchmark.cpp
```

CMake 开关位置：

```text
CMakeLists.txt
src/C++/CMakeLists.txt
```

### 2.3 编译开关和运行开关

编译期开关：

```cmake
option(QUICKFIX_FIXED_LAYOUT_PARSER "Enable experimental fixed-layout parser fast paths" OFF)
```

打开后会给 quickfix library 添加：

```cmake
target_compile_definitions(${PROJECT_NAME} PUBLIC QUICKFIX_FIXED_LAYOUT_PARSER)
```

benchmark 的运行期开关：

```bash
--fixed-layout
```

两者的关系是：

- `QUICKFIX_FIXED_LAYOUT_PARSER=ON`：编译出 fixed-layout 快速解析能力。
- `--fixed-layout`：benchmark 发送带 `9001` 的固定模板消息。

如果只编译打开 fixed-layout，但运行命令不带 `--fixed-layout`，benchmark 发送的还是普通 FIX 消息，服务端会因为没有固定 offset 的 marker 而走普通解析。

### 2.4 触发条件

fixed-layout 快速路径在 `Message::setString()` 一开始尝试：

```cpp
#if defined(QUICKFIX_FIXED_LAYOUT_PARSER)
  if (setFixedLayoutString(string)) {
    return;
  }
#endif
```

也就是说，一条消息进入 `Message::setString()` 后，会先尝试模板匹配。当前匹配条件只有两类：

1. 整条消息长度必须等于模板长度。
2. 固定 offset 上必须匹配模板 marker。

marker 检查位置固定：

```cpp
FIXED_LAYOUT_MARKER_FIELD_OFFSET = 78
FIXED_LAYOUT_MARKER_FIELD_SIZE = 10
```

也就是直接比较完整消息中 offset `78` 开始的 10 个字节：

```text
9001=NOS1<SOH>
9001=CXL1<SOH>
9001=MDW1<SOH>
```

当前不会扫描整条消息去找 `9001`，这是故意的。因为扫描会重新引入额外成本。真正的固定模板消息应该保证 `9001` 就出现在固定 offset 上。

当前三种触发条件是：

| 模板 | 消息类型 | MsgType | 总长度 | BodyLength | marker |
|---|---|---:|---:|---:|---|
| `NOS1` | NewOrderSingle | `D` | 187 | 164 | `9001=NOS1<SOH>` |
| `CXL1` | OrderCancelRequest | `F` | 185 | 162 | `9001=CXL1<SOH>` |
| `MDW1` | MarketDataSnapshotFullRefresh | `W` | 288 | 265 | `9001=MDW1<SOH>` |

如果长度或 marker 任意一个不匹配，`setFixedLayoutString()` 返回 `false`，然后继续走 QuickFIX 原来的普通解析路径。

### 2.5 命中后怎么解析

模板命中后，代码不再对整条消息做普通字段循环，而是使用预设表：

```cpp
struct FixedFieldSpec {
  int tag;
  std::string::size_type fieldOffset;
  std::string::size_type valueOffset;
  std::string::size_type valueLength;
  FixedFieldTarget target;
};
```

每个字段都提前写死：

- `tag`：字段号
- `fieldOffset`：完整 wire string 中 tag 开始的位置
- `valueOffset`：完整 wire string 中 value 开始的位置
- `valueLength`：value 的固定长度
- `target`：放入 header、body 还是 trailer

命中后用这些 offset 直接构造 `FieldBase`：

```cpp
const std::string::const_iterator tagStart = string.begin() + fieldOffset;
const std::string::const_iterator valueStart = string.begin() + valueOffset;
const std::string::const_iterator valueEnd = valueStart + valueLength;
return FieldBase(tag, valueStart, valueEnd, tagStart, valueEnd + 1);
```

然后按 `target` 放入：

- `m_header`
- `Message` body
- `m_trailer`

行情快照 `MDW1` 还会额外按固定 offset 构造 3 个 `NoMDEntries(268)` repeating group。

### 2.6 NOS1 下单模板

wire 结构：

```text
8=FIX.4.2<SOH>
9=164<SOH>
35=D<SOH>
49=CLIENT<SOH>
56=SERVER<SOH>
34=<12位序号><SOH>
52=<17位发送时间><SOH>
9001=NOS1<SOH>
11=ORDER-<12位序号><SOH>
21=1<SOH>
55=LNUX<SOH>
54=1<SOH>
60=<17位交易时间><SOH>
38=0000000100<SOH>
40=1<SOH>
59=0<SOH>
15=USD<SOH>
10=<3位校验和><SOH>
```

总长度固定为 `187` 字节。

字段 offset 表：

| tag | 含义 | fieldOffset | valueOffset | valueLength | target | value 模板 |
|---:|---|---:|---:|---:|---|---|
| 8 | BeginString | 0 | 2 | 7 | Header | `FIX.4.2` |
| 9 | BodyLength | 10 | 12 | 3 | Header | `164` |
| 35 | MsgType | 16 | 19 | 1 | Header | `D` |
| 49 | SenderCompID | 21 | 24 | 6 | Header | `CLIENT` |
| 56 | TargetCompID | 31 | 34 | 6 | Header | `SERVER` |
| 34 | MsgSeqNum | 41 | 44 | 12 | Header | 12 位补零序号 |
| 52 | SendingTime | 57 | 60 | 17 | Header | `YYYYMMDD-HH:MM:SS` |
| 9001 | FixedLayoutMarker | 78 | 83 | 4 | Body | `NOS1` |
| 11 | ClOrdID | 88 | 91 | 18 | Body | `ORDER-` + 12 位序号 |
| 21 | HandlInst | 110 | 113 | 1 | Body | `1` |
| 55 | Symbol | 115 | 118 | 4 | Body | `LNUX` |
| 54 | Side | 123 | 126 | 1 | Body | `1` |
| 60 | TransactTime | 128 | 131 | 17 | Body | `YYYYMMDD-HH:MM:SS` |
| 38 | OrderQty | 149 | 152 | 10 | Body | `0000000100` |
| 40 | OrdType | 163 | 166 | 1 | Body | `1` |
| 59 | TimeInForce | 168 | 171 | 1 | Body | `0` |
| 15 | Currency | 173 | 176 | 3 | Body | `USD` |
| 10 | CheckSum | 180 | 183 | 3 | Trailer | 3 位 checksum |

### 2.7 CXL1 撤单模板

wire 结构：

```text
8=FIX.4.2<SOH>
9=162<SOH>
35=F<SOH>
49=CLIENT<SOH>
56=SERVER<SOH>
34=<12位序号><SOH>
52=<17位发送时间><SOH>
9001=CXL1<SOH>
41=ORIG-<12位原始序号><SOH>
11=CNCL-<12位序号><SOH>
55=LNUX<SOH>
54=1<SOH>
60=<17位交易时间><SOH>
38=0000000100<SOH>
10=<3位校验和><SOH>
```

总长度固定为 `185` 字节。

字段 offset 表：

| tag | 含义 | fieldOffset | valueOffset | valueLength | target | value 模板 |
|---:|---|---:|---:|---:|---|---|
| 8 | BeginString | 0 | 2 | 7 | Header | `FIX.4.2` |
| 9 | BodyLength | 10 | 12 | 3 | Header | `162` |
| 35 | MsgType | 16 | 19 | 1 | Header | `F` |
| 49 | SenderCompID | 21 | 24 | 6 | Header | `CLIENT` |
| 56 | TargetCompID | 31 | 34 | 6 | Header | `SERVER` |
| 34 | MsgSeqNum | 41 | 44 | 12 | Header | 12 位补零序号 |
| 52 | SendingTime | 57 | 60 | 17 | Header | `YYYYMMDD-HH:MM:SS` |
| 9001 | FixedLayoutMarker | 78 | 83 | 4 | Body | `CXL1` |
| 41 | OrigClOrdID | 88 | 91 | 17 | Body | `ORIG-` + 12 位原始序号 |
| 11 | ClOrdID | 109 | 112 | 17 | Body | `CNCL-` + 12 位序号 |
| 55 | Symbol | 130 | 133 | 4 | Body | `LNUX` |
| 54 | Side | 138 | 141 | 1 | Body | `1` |
| 60 | TransactTime | 143 | 146 | 17 | Body | `YYYYMMDD-HH:MM:SS` |
| 38 | OrderQty | 164 | 167 | 10 | Body | `0000000100` |
| 10 | CheckSum | 178 | 181 | 3 | Trailer | 3 位 checksum |

### 2.8 MDW1 行情快照模板

wire 结构：

```text
8=FIX.4.2<SOH>
9=265<SOH>
35=W<SOH>
49=CLIENT<SOH>
56=SERVER<SOH>
34=<12位序号><SOH>
52=<17位发送时间><SOH>
9001=MDW1<SOH>
262=MDREQ-<12位序号><SOH>
55=LNUX<SOH>
268=3<SOH>
269=0<SOH>
270=00000123.4500<SOH>
271=0000000500<SOH>
273=12:34:56<SOH>
269=1<SOH>
270=00000123.4600<SOH>
271=0000000400<SOH>
273=12:34:56<SOH>
269=2<SOH>
270=00000123.4550<SOH>
271=0000000100<SOH>
273=12:34:56<SOH>
10=<3位校验和><SOH>
```

总长度固定为 `288` 字节。

普通字段 offset 表：

| tag | 含义 | fieldOffset | valueOffset | valueLength | target | value 模板 |
|---:|---|---:|---:|---:|---|---|
| 8 | BeginString | 0 | 2 | 7 | Header | `FIX.4.2` |
| 9 | BodyLength | 10 | 12 | 3 | Header | `265` |
| 35 | MsgType | 16 | 19 | 1 | Header | `W` |
| 49 | SenderCompID | 21 | 24 | 6 | Header | `CLIENT` |
| 56 | TargetCompID | 31 | 34 | 6 | Header | `SERVER` |
| 34 | MsgSeqNum | 41 | 44 | 12 | Header | 12 位补零序号 |
| 52 | SendingTime | 57 | 60 | 17 | Header | `YYYYMMDD-HH:MM:SS` |
| 9001 | FixedLayoutMarker | 78 | 83 | 4 | Body | `MDW1` |
| 262 | MDReqID | 88 | 92 | 18 | Body | `MDREQ-` + 12 位序号 |
| 55 | Symbol | 111 | 114 | 4 | Body | `LNUX` |
| 10 | CheckSum | 281 | 284 | 3 | Trailer | 3 位 checksum |

行情 repeating group 固定为 `268=3`，并构造 3 组 `269/270/271/273`：

| group | tag | 含义 | fieldOffset | valueOffset | valueLength | value 模板 |
|---:|---:|---|---:|---:|---:|---|
| 1 | 269 | MDEntryType | 125 | 129 | 1 | `0` |
| 1 | 270 | MDEntryPx | 131 | 135 | 13 | `00000123.4500` |
| 1 | 271 | MDEntrySize | 149 | 153 | 10 | `0000000500` |
| 1 | 273 | MDEntryTime | 164 | 168 | 8 | `12:34:56` |
| 2 | 269 | MDEntryType | 177 | 181 | 1 | `1` |
| 2 | 270 | MDEntryPx | 183 | 187 | 13 | `00000123.4600` |
| 2 | 271 | MDEntrySize | 201 | 205 | 10 | `0000000400` |
| 2 | 273 | MDEntryTime | 216 | 220 | 8 | `12:34:56` |
| 3 | 269 | MDEntryType | 229 | 233 | 1 | `2` |
| 3 | 270 | MDEntryPx | 235 | 239 | 13 | `00000123.4550` |
| 3 | 271 | MDEntrySize | 253 | 257 | 10 | `0000000100` |
| 3 | 273 | MDEntryTime | 268 | 272 | 8 | `12:34:56` |

注意：`268=3` 本身在当前实现里没有放入 `FIXED_MDW1_FIELDS` 的普通字段表，而是通过 `addGroup(group)` 后由 `FieldMap` 的 group 结构维护 group count。对上层读取 repeating group 来说，它仍然表现为 3 组行情条目。

### 2.9 fallback 行为

fixed-layout 不是替代全部 FIX 解析，而是一个前置快速尝试。

下面几种情况都会回退普通解析：

- 没有编译 `QUICKFIX_FIXED_LAYOUT_PARSER`。
- 消息没有固定模板 marker。
- `9001` 不在 offset `78`。
- marker 是未知值。
- 消息总长度不等于模板长度。
- benchmark 命令没有带 `--fixed-layout`，发送的是普通消息。

回退后仍然由原来的 `Message::setString()` 字段循环处理，所以普通 FIX 消息不受影响。

benchmark 里也专门加了 fallback self-test：

- 长度不匹配但仍带 `9001=NOS1`：必须回退普通解析并解析成功。
- 长度相同但 `9001=NOS1` 不在固定 offset：必须回退普通解析并解析成功。

### 2.10 正确性和风险边界

fixed-layout 命中后会直接按 offset 构造字段，并在 `Message::setString()` 中提前返回。这意味着它跳过了普通解析里的很多通用工作：

- 不逐字段找 `=`
- 不逐字段找 `SOH`
- 不逐字段把 tag 文本转整数
- 不通过数据字典动态识别 repeating group
- 不执行 `Message::validate()` 里的 BodyLength / CheckSum 通用校验

这是这个优化的性能来源，也是它的使用边界。

因此它适合可信来源、格式完全由我们控制的高频消息。当前触发条件故意只做长度和 marker 检查，不做更多字段内容校验。如果一条消息长度和 marker 都匹配，但内部某个固定字段内容被破坏，fixed-layout 仍会按模板 offset 提取字段。

对实验来说，这符合我们设定的目标：带自定义 tag 的固定模板消息由 benchmark 或受控发送端生成，协议双方约定这些消息满足固定布局。

### 2.11 对性能路径的影响

普通解析路径大致是：

```text
wire string
  -> Message::setString()
  -> extractField()
  -> find '='
  -> find SOH
  -> tag convert
  -> header/body/trailer 判断
  -> repeating group 判断
  -> optional validate()
```

fixed-layout 命中后的路径是：

```text
wire string
  -> Message::setString()
  -> setFixedLayoutString()
  -> length + marker match
  -> 按 offset 构造 FieldBase
  -> 直接 append 到 header/body/trailer/group
```

也就是说，它主要减少的是 `Message` 层字段解析成本，不负责网络读取，也不负责从 socket stream 切完整 FIX 消息。

因此它和 `QUICKFIX_SIMD_STREAM_PARSER` 是独立但兼容的：

- SIMD stream parser：负责从连续 socket buffer 中切出完整 FIX 消息。
- fixed-layout parser：负责拿到完整 FIX 消息后，按 offset 快速构造 `Message`。

server/raw + fixed-layout + SIMD 的组合路径是：

```text
raw client 发固定模板 wire bytes
  -> server socket recv
  -> Parser::readFixMessage() 可用 SIMD 切完整消息
  -> Message::setString() 命中 fixed-layout
  -> Application::fromApp()
```

### 2.12 benchmark 使用方式

只测试 fixed-layout parser：

```bash
cmake -S . -B build-bench-fixed-only \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=OFF \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=OFF \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=ON \
  -DQUICKFIX_BUSY_POLL=OFF

cmake --build build-bench-fixed-only --target fix_parse_benchmark_target -j 2
```

正确性测试：

```bash
./test/run_parse_benchmark.sh --self-test-correctness --fixed-layout --messages=1000
```

parse mode 只看 `Message::setString()` 层面的解析速度：

```bash
./test/run_parse_benchmark.sh --mode=parse --message=new-order-single --fixed-layout --messages=1000000
./test/run_parse_benchmark.sh --mode=parse --message=order-cancel-request --fixed-layout --messages=1000000
./test/run_parse_benchmark.sh --mode=parse --message=market-data-snapshot --fixed-layout --messages=1000000
```

server/raw mode 测完整服务端收包、切包、解析路径：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --fixed-layout --messages=1000000 --port=54331
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --fixed-layout --messages=1000000 --port=54332
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --fixed-layout --messages=1000000 --port=54333
```

当前 server mode 下 `--fixed-layout` 只支持 `--client=raw`。如果使用 `--client=quickfix`，benchmark 会直接报错。这是为了保证发送出去的 wire bytes 完全等于固定模板；QuickFIX initiator 可能重排或重新序列化字段，从而破坏固定 offset。

### 2.13 当前结论

fixed-layout parser 是一个面向少数高频、强约束消息的专用快速路径。

它不追求覆盖所有 FIX 消息，而是给下单、撤单、行情快照这三类最常见消息提供可控实验入口。普通消息、未知消息、长度或 marker 不匹配的消息都会回退 QuickFIX 原始解析。

它和 SIMD stream parser、busy-poll 网络模式互相独立，可以单独开关，也可以组合测试。

## 3. Busy-Poll Network Loop

### 3.1 优化目标

QuickFIX 原来的 socket acceptor 路径在 Linux/WSL 下使用 `poll()` 等待 socket 事件。socket 本身会被设置成 non-blocking，但事件等待是阻塞式的：

```text
non-blocking socket
  -> poll(..., timeout)
  -> 有事件后 recv/send/accept
```

这意味着没有网络事件时，acceptor 线程会睡在 `poll()` 里。这样 CPU 占用低，但事件到达后需要经历一次内核唤醒调度。

busy-poll 的目标是给低延迟/高吞吐实验提供一条可切换路径：

```text
non-blocking socket
  -> poll(..., 0)
  -> 没事件也立刻返回
  -> acceptor 外层循环继续调用 poll(..., 0)
```

也就是说，当前实现的 busy-poll 是用户态 event loop 层面的忙轮询，不是 Linux `SO_BUSY_POLL` socket option。它的核心是 `poll` 零等待，不让 acceptor 线程在内核里长时间睡眠。

### 3.2 当前已实现范围

当前已经完成的是第一阶段：

- 编译期开关：`QUICKFIX_BUSY_POLL`
- 运行期开关：`SocketBusyPoll=Y`
- benchmark 参数：`--busy-poll`
- 可选绑核：`SocketBusyPollCpu=N`
- benchmark 参数：`--busy-poll-cpu=N`

当前实现只改 acceptor 网络等待方式，不改变：

- FIX 消息格式
- `Parser::readFixMessage()` 切包逻辑
- `Message::setString()` 字段解析逻辑
- `MessageCracker` 分发逻辑
- `Application::fromApp()` 行为

所以它和 SIMD、fixed-layout 是独立优化点。

### 3.3 改动位置

CMake 开关：

```text
CMakeLists.txt
src/C++/CMakeLists.txt
```

配置项：

```text
src/C++/SessionSettings.h
```

网络等待核心：

```text
src/C++/SocketMonitor_UNIX.h
src/C++/SocketMonitor_UNIX.cpp
```

SocketServer 转发开关：

```text
src/C++/SocketServer.h
src/C++/SocketServer.cpp
```

SocketAcceptor 读取配置和绑核：

```text
src/C++/SocketAcceptor.h
src/C++/SocketAcceptor.cpp
```

benchmark 参数和配置生成：

```text
src/fix_parse_benchmark.cpp
```

Windows 侧：

```text
src/C++/SocketMonitor_WIN32.h
src/C++/SocketMonitor_WIN32.cpp
```

Windows 目前只保留 `setBusyPoll(bool)` no-op，用于接口一致；当前真正的 busy-poll 行为是在 UNIX/WSL 的 `poll()` 路径上实现的。

### 3.4 编译开关和运行开关

编译期开关：

```cmake
option(QUICKFIX_BUSY_POLL "Enable experimental busy-poll socket monitor mode" OFF)
```

打开后会给 quickfix library 添加：

```cmake
target_compile_definitions(${PROJECT_NAME} PUBLIC QUICKFIX_BUSY_POLL)
```

新增配置项：

```cpp
const char SOCKET_BUSY_POLL[] = "SocketBusyPoll";
const char SOCKET_BUSY_POLL_CPU[] = "SocketBusyPollCpu";
```

benchmark 运行参数：

```bash
--busy-poll
--busy-poll-cpu N
```

benchmark 会把它们写入 acceptor 配置：

```text
SocketBusyPoll=Y
SocketBusyPollCpu=N
```

如果二进制没有用 `-DQUICKFIX_BUSY_POLL=ON` 编译，即使命令行带 `--busy-poll`，配置也会被读取，但核心 `SocketMonitor` 不会启用零等待 busy-poll 行为。因此实验时需要确认 build profile 确实打开了 `QUICKFIX_BUSY_POLL=ON`。

### 3.5 原始路径是什么样

普通 `SocketAcceptor` 启动后会进入：

```cpp
while (!isStopped() && m_pServer && m_pServer->block(*this)) {}
```

`SocketServer::block()` 再调用：

```cpp
m_monitor.block(wrapper, poll, timeout);
```

Linux/WSL 下 `SocketMonitor::block()` 会构造 `pollfd` 数组，然后调用：

```cpp
poll(pfds, pfds_size, getTimeval(should_poll, timeout));
```

`SocketServer` 初始化时当前使用 `SocketServer(1)`，所以普通 acceptor 线程在没有事件时最多按大约 1 秒粒度阻塞在 `poll()` 上。socket 本身是 non-blocking，但线程等待事件这一步是阻塞的。

### 3.6 当前 busy-poll 怎么改

当前核心改动在 `SocketMonitor_UNIX.cpp`：

```cpp
result = poll(pfds, pfds_size, m_busyPoll ? 0 : getTimeval(should_poll, timeout));
```

也就是说：

- `m_busyPoll=false`：保持原始阻塞式 `poll`。
- `m_busyPoll=true`：使用 `poll(..., 0)`，立即返回。

如果 `poll(..., 0)` 返回 0，说明当前这一轮没有事件。普通模式会触发 `onTimeout()`，但 busy-poll 模式如果每一轮都触发 timeout，会导致心跳/定时逻辑被高频调用，反而制造大量额外开销。

所以当前加了一个节流函数：

```cpp
bool SocketMonitor::busyPollTimeoutElapsed()
```

busy-poll 模式下只有距离上次 timeout 已经超过 `m_timeout`，才真正调用：

```cpp
strategy.onTimeout(*this);
```

这样可以保持 session timeout/heartbeat 逻辑仍然被周期性驱动，同时避免每次空轮询都调用 timeout。

### 3.7 绑核怎么实现

绑核发生在 `SocketAcceptor::onStart()`，也就是 acceptor 网络线程启动后：

```cpp
if (m_busyPoll && m_busyPollCpu >= 0) {
  setCurrentThreadAffinity(m_busyPollCpu);
}
```

底层调用：

```cpp
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
```

因此 `--busy-poll-cpu=N` 绑定的是当前 `SocketAcceptor` 网络线程，不是整个进程。

这点很重要：

- acceptor 网络线程可以被绑到 CPU N。
- benchmark 的 raw client 主线程仍然可能被操作系统调度到其他 CPU。
- 如果整个进程再被 `taskset` 限制到很小的 CPU 集合，client 和 acceptor 仍可能互相抢 CPU。

更严谨的后续实验可以继续增加 `--client-cpu`，把 raw client 主线程也显式绑到另一个 CPU。当前第一阶段还没有实现这个参数。

### 3.8 如何确认 busy-poll 和绑核生效

benchmark 输出里会出现：

```text
busy_poll=yes
busy_poll_cpu=2
```

这说明 benchmark 已经把 busy-poll 配置写进 acceptor 配置，并且请求绑定 CPU 2。

更直接的观察方式是看 CPU 占用。打开 `top` 后按 `1` 展开每个 CPU：

```bash
top
```

如果运行：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=5000000 --port=54380 --busy-poll --busy-poll-cpu=2
```

预期现象是 `Cpu2` 的 idle 接近 `0.0`，`us + sy` 接近 `100%`。这说明 busy-poll 线程大概率已经在 CPU 2 上持续运行。

也可以用：

```bash
mpstat -P ALL 1
```

把 `--busy-poll-cpu=2` 改成 `--busy-poll-cpu=1` 后，高占用 CPU 应该跟着从 CPU 2 转移到 CPU 1，这是确认绑核最直接的对照。

### 3.9 对性能和资源的影响

busy-poll 的收益来自减少阻塞等待和线程唤醒延迟，代价是持续消耗 CPU。

可能带来的正面影响：

- 网络事件到达后更快被 acceptor 线程发现。
- 在高频消息压测中减少睡眠/唤醒调度影响。
- 和固定绑核结合后，实验环境更稳定。

可能带来的负面影响：

- 会吃满一个 CPU core。
- 在同进程 benchmark 中，acceptor 和 raw client 可能抢 CPU。
- 如果消息量不够大，收益可能被线程调度、发送端、解析成本或 WSL 网络栈噪声掩盖。
- 如果瓶颈在 `Message::setString()`、数据字典校验或应用层处理，busy-poll 不一定提升总吞吐。

因此网络层实验建议使用较大的消息数，例如：

```bash
--messages=1000000
```

或：

```bash
--messages=5000000
```

10 万条消息可以用于 smoke test，但观察 busy-poll 性能差异可能偏短。

### 3.10 当前第一阶段和普通路径的关系

当前实现保留了普通路径。

同一个 `QUICKFIX_BUSY_POLL=ON` build 里：

- 带 `--busy-poll`：运行期启用 `poll(..., 0)`。
- 不带 `--busy-poll`：运行期仍走原始阻塞式 `poll`。

所以不用为了比较 busy-poll on/off 每次都重新 build。只要当前二进制是 busy-poll-capable build，运行时去掉 `--busy-poll` 就能得到同一二进制下的阻塞 `poll` baseline。

如果要和完全未编译 busy-poll 的原始 build 比较，再使用：

```bash
-DQUICKFIX_BUSY_POLL=OFF
```

单独构建 baseline。

### 3.11 当前第一阶段没有改变什么

第一阶段只改变等待事件的方式。

它没有改变当前 readiness 到达后的读取策略。现在 `SocketConnection::readFromSocket()` 仍然是：

```cpp
char m_buffer[BUFSIZ];
ssize_t size = socket_recv(m_socket, m_buffer, sizeof(m_buffer));
m_parser.addToStream(m_buffer, size);
```

在当前 WSL/glibc 环境里，`BUFSIZ` 是 `8192`，所以一次 `recv()` 最多从 socket 读 8 KiB 到用户态。之后 `readMessages()` 会从 parser buffer 里尽量解析出多条完整 FIX 消息。

也就是说，第一阶段做的是：

```text
更积极地发现 socket 可读
```

但不是：

```text
一次可读事件后尽可能把 socket 内核缓冲区读空
```

这正是后续 read-drain 实验可以继续做的事情。

### 3.12 未来 read-drain 模式

后续可以在 busy-poll 模式下增加 read-drain 策略。

当前逻辑是一次 socket readable 事件后只执行一次 `recv()`：

```text
poll 发现 fd 可读
  -> recv 8 KiB
  -> addToStream
  -> readMessages
  -> 返回 event loop
```

read-drain 可以改成：

```text
poll 发现 fd 可读
  -> recv 8 KiB
  -> recv 8 KiB
  -> recv 8 KiB
  -> ...
  -> 直到 EAGAIN / EWOULDBLOCK，或达到 read budget
  -> addToStream / readMessages
  -> 返回 event loop
```

这样做的目标是：当内核 socket receive buffer 里已经堆了很多 FIX 消息时，不要每读 8 KiB 就回到 `poll` 一次，而是在一次 readable 事件里多读几批。

可选设计：

- 新增编译开关：`QUICKFIX_BUSY_POLL_READ_DRAIN`
- 新增配置项：`SocketBusyPollReadDrain`
- 新增配置项：`SocketBusyPollReadDrainMaxReads`
- 新增配置项：`SocketBusyPollReadDrainMaxBytes`
- benchmark 参数：`--busy-poll-read-drain`
- benchmark 参数：`--busy-poll-read-drain-max-reads N`

建议默认给 read-drain 设置上限，例如：

```text
max_reads = 16
max_bytes = 128 KiB
```

原因是如果某个连接特别活跃，无限制读空可能导致其他连接被饿住。加 budget 可以在吞吐和公平性之间取一个实验上可控的平衡。

read-drain 可能改善的场景：

- raw client 一次性快速发送大量 FIX 消息。
- socket receive buffer 经常积压超过 8 KiB。
- 当前性能受 repeated poll / repeated event loop 开销影响。

read-drain 可能带来的问题：

- 更多 `recv()` 调用可能读到 `EAGAIN`，空读增加开销。
- 单连接 drain 太久可能影响多连接公平性。
- 如果瓶颈在 FIX 解析本身，而不是 socket 读取，收益可能有限。

### 3.13 direct recv scan / poll-bypass 模式

direct 模式比 `poll0` 更激进：acceptor 网络线程不再每轮调用 `poll()`，而是直接尝试 `accept()` 和
`recv()`。当前实现是独立实验分支，不会替换 blocking 或 `poll0` 路径。

概念路径：

```text
没有活跃连接
  -> 直接尝试 non-blocking accept
有一个活跃连接
  -> 直接尝试 non-blocking recv
  -> recv 成功就送入原 Parser 和 Session
  -> EAGAIN 表示本轮暂时没数据，立即进入下一轮
每秒
  -> 执行一次原有 session timeout 处理
```

这个模式接近一些专门网络循环项目的思路：当系统明确处于高频收包状态时，直接尝试从 fd 读数据，减少
`poll` syscall 和 pollfd 构建/遍历成本。

但这个阶段风险也明显更高：

- 空闲连接多时会产生大量 `EAGAIN`。
- 需要自己处理公平性、错误事件、断连、写事件和 accept 新连接。
- 很容易让 CPU 花在“扫空 fd”上。
- 实现复杂度高于 `poll0` 和 read-drain。

#### direct 模式阶段一：底层直读封装

阶段一只建立可测试的 non-blocking 直读基础：

- 新增默认关闭的编译开关：`QUICKFIX_DIRECT_READ_POLL`。
- 新增独立实现：`src/C++/detail/DirectSocketRead.h/.cpp`。
- non-blocking `recv()` 的结果被明确区分为 `Data`、`WouldBlock`、`PeerClosed` 和 `Error`。
- `EAGAIN` / `EWOULDBLOCK` 表示当前 fd 暂时没有数据，不会被误判为断线。
- `EINTR` 会在接收函数内部重试。
- 新增 socketpair 测试，覆盖数据、EAGAIN、对端关闭、真实错误和分片 FIX 消息。

阶段一没有接入 acceptor 主循环，用来先证明 `EAGAIN`、断连、错误和分片数据的语义正确。

阶段一验证命令：

```bash
cmake -S . -B build-direct-stage1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_LIB_OUTPUT_DIR="$PWD/build-direct-stage1/out" \
  -DQUICKFIX_EXAMPLES=OFF \
  -DQUICKFIX_TESTS=ON \
  -DQUICKFIX_SHARED_LIBS=OFF \
  -DQUICKFIX_DIRECT_READ_POLL=ON

cmake --build build-direct-stage1 --target ut --parallel 2
./build-direct-stage1/out/ut DirectSocketReadTests
./build-direct-stage1/out/ut --quickfix-spec-path spec
```

#### direct 模式阶段二：单连接 acceptor 主循环

阶段二已经把 direct scan 接入 `SocketAcceptor`，但刻意限制为**一个活跃连接**，先验证端到端正确性：

- 新增配置项 `SocketBusyPollMode=blocking|poll0|direct`，默认仍是 `blocking`。
- `direct` 需要 UNIX 构建并设置 `QUICKFIX_DIRECT_READ_POLL=ON`。
- `SocketAcceptor::onStart()` 只在 `direct` 模式进入 `runDirectScanOnce()`；其余模式继续调用原来的
  `SocketServer::block()`。
- 没有连接时，`SocketServer::acceptDirect()` 直接尝试 non-blocking `accept()`。
- 有连接时，`SocketConnection::readDirect()` 每轮直接执行一次 non-blocking `recv()`。
- `EAGAIN` / `EWOULDBLOCK` 只是表示本轮没有数据，马上进入下一轮，不会断开连接。
- 分片 Logon 会累计进原有 `Parser` buffer；只有完整 Logon 到达后才建立 Session。
- 每秒仍调用一次现有 timeout 处理，避免 direct 循环丢失 session 定时任务。
- `SocketBusyPollCpu=N` 同样可以把 direct acceptor 线程绑定到 CPU N。
- direct 断开使用独立的立即清理入口，不会向仅由原 poll 路径消费的断开队列留下事件。
- 写方向暂时继续使用 QuickFIX 原有同步发送和队列逻辑。

三条路径的顶层关系是：

```text
SocketAcceptor::onStart()
  -> SocketBusyPollMode=direct
       -> runDirectScanOnce()
          -> acceptDirect() / readDirect()
  -> 其他模式
       -> SocketServer::block()
          -> blocking poll 或 poll(..., 0)
```

因此 direct 分支不会改写 `SocketMonitor::block()`、`SocketServer::block()`、`SocketConnection::read()`，原始
blocking 和第一阶段 `poll0` busy-poll 仍可作为同一二进制中的 baseline。

阶段二构建与验证命令：

```bash
cmake -S . -B build-direct-stage2 \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_LIB_OUTPUT_DIR="$PWD/build-direct-stage2/out" \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_TESTS=ON \
  -DQUICKFIX_SHARED_LIBS=OFF \
  -DQUICKFIX_BUSY_POLL=ON \
  -DQUICKFIX_DIRECT_READ_POLL=ON

cmake --build build-direct-stage2 --target ut --parallel 2
./build-direct-stage2/out/ut DirectSocketReadTests
./build-direct-stage2/out/ut DirectSocketAcceptorTests
./build-direct-stage2/out/ut --quickfix-spec-path spec
```

`DirectSocketAcceptorTests` 覆盖分片 Logon、应用消息、对端断开以及同一 Session 的顺序重连。

阶段二当前边界：

- 只面向普通 UNIX `SocketAcceptor`，不覆盖 Windows、SSL acceptor、threaded acceptor 或 initiator。
- 只处理一个活跃连接；多连接轮转、公平性和 accept/read budget 留到后续阶段。
- 每轮只尝试一次 `recv()`，还没有实现“连续读取直到 EAGAIN”的 read-drain。
- 还没有为 direct 模式单独重做写就绪和 partial-write 重试策略。

#### direct 模式阶段三：多连接扫描与 direct 写队列

阶段三在不调用 `poll()` 的前提下补回原 blocking/poll0 路径已有的多连接语义：

- 当当前连接数小于已配置 Session 数量时，每轮最多直接尝试一次 non-blocking `accept()`。
- 每轮遍历全部活跃连接，每个连接最多调用一次 `readDirect()`。
- 一个连接返回 `EAGAIN` 后立即检查下一个连接，空闲连接不会阻止其他连接读取。
- 遍历时预先推进 map 迭代器，因此当前连接断开并从 `m_connections` 删除后仍可继续扫描。
- 每个连接每轮最多推进一次 direct 发送队列；partial write 和 `EAGAIN` 保留偏移并在下一轮继续。
- direct 连接发送时不再向仅由原 poll 路径消费的 monitor signal socket 写通知。
- 单 Session direct 性能路径不会额外执行 accept：连接数达到配置 Session 数后会跳过 accept 尝试。

阶段三一轮扫描的结构：

```text
连接数 < 配置 Session 数
  -> acceptDirect() 一次
遍历 m_connections
  -> connection A: readDirect() 一次 + processQueueDirect() 一次
  -> connection B: readDirect() 一次 + processQueueDirect() 一次
  -> connection C: readDirect() 一次 + processQueueDirect() 一次
  -> Data: Parser/Session
  -> EAGAIN: 继续下一个连接
  -> close/error: 安全删除当前连接
每秒
  -> 原 Session timeout
```

阶段三仍然不是 read-drain。同一热连接不会在一轮里反复读取到 `EAGAIN`，因此多连接实验不会同时混入
“单连接连续读空”这个变量。

阶段三构建与验证命令：

```bash
cmake -S . -B build-direct-stage3 \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_LIB_OUTPUT_DIR="$PWD/build-direct-stage3/out" \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_TESTS=ON \
  -DQUICKFIX_SHARED_LIBS=OFF \
  -DQUICKFIX_BUSY_POLL=ON \
  -DQUICKFIX_DIRECT_READ_POLL=ON

cmake --build build-direct-stage3 --target ut fix_parse_benchmark_target --parallel 2
./build-direct-stage3/out/ut DirectSocketReadTests
./build-direct-stage3/out/ut DirectSocketAcceptorTests
./build-direct-stage3/out/ut DirectMultiSocketAcceptorTests
./build-direct-stage3/out/ut --quickfix-spec-path spec
```

`DirectMultiSocketAcceptorTests` 使用两个不同 FIX Session，覆盖交错/分片 Logon、热点连接下的公平读取、
跨轮发送队列、单连接断开不影响另一连接，以及断开 Session 的再次接入。

阶段三当前边界：

- 最大同时接入数按配置 Session 数量限制，没有新增独立的 `MaxConnections` 配置。
- 每轮扫描所有连接；大量空闲 Session 会产生相应数量的 `EAGAIN`，尚未实现 scan budget 或空闲退避。
- 仍不覆盖 Windows、SSL acceptor、threaded acceptor 或 initiator 的网络循环。
- 现有 benchmark 每次仍创建一个 client；多连接正确性由集成测试覆盖，多连接吞吐工具留待后续扩展。

#### direct 模式阶段四：非阻塞发送、会话定时器与正常停止

阶段四没有增加新的运行参数。只要构建时启用了 `QUICKFIX_DIRECT_READ_POLL`，运行时选择
`SocketBusyPollMode=direct` 或 benchmark 的 `--direct-read-poll`，就会自动使用下面的发送和定时路径。

发送侧实现分为两层：

- `src/C++/detail/DirectSocketWrite.h/.cpp` 每次直接尝试一次 non-blocking `send()`，重试 `EINTR`，并明确返回
  `Progress`、`WouldBlock`、`PeerClosed` 或 `Error`。
- `SocketConnection::processQueueDirect()` 只在 `Progress` 时增加当前消息偏移；完整消息才从队列弹出。
- 部分发送会保留未发送后缀；`EAGAIN/EWOULDBLOCK` 会保留消息和偏移，下一轮 direct scan 继续。
- `EPIPE`、`ECONNRESET`、`ENOTCONN` 和其他硬错误会返回断开，不会把失败消息误判为发送完成。
- direct 模式的 `SocketConnection::send()` 只入队，不向原 poll 路径的 signal socket 写通知。

定时侧继续复用 QuickFIX 原来的 `Session::next()` 状态机，但由 direct 网络循环调度：

- `SocketAcceptor::m_directTimeoutTick` 使用 `std::chrono::steady_clock`，每秒调用一次 `onTimeout()`。
- `onTimeout()` 进入 `SocketConnection::onTimeout()`，再调用 `Session::next()`，由原 Session 状态机决定是否生成
  Heartbeat、TestRequest、Logout 或执行 Logout 超时断开。
- 尚未识别 Session 的连接使用 `m_directLogonDeadline`，同样用 `steady_clock` 限制等待 Logon 的时间。
- 单调时钟负责本地调度间隔；FIX 字段 `SendingTime(52)` 仍使用 UTC，这是协议要求的线上时间戳。

阶段四专项验证：

- `DirectSocketWriteTests` 使用真实 non-blocking socket 和 4 KiB 发送缓冲，确认实际发生部分写入和 `EAGAIN`；
  读取端恢复后，4 MiB 数据逐字节一致，并验证对端关闭与硬错误分类。
- `DirectSocketWriteBackpressureTests` 通过真实 `SocketAcceptor` 发送 4 MiB FIX News。慢客户端期间 Session 保持登录，
  最终 wire message 完整一致，随后同一连接还能继续处理订单。
- `DirectSocketSessionTimerTests` 端到端验证自动 Heartbeat、TestRequest、带 `TestReqID=TEST` 的响应、Logout 发送
  以及未收到 Logout 回应时的超时断开。
- `DirectSocketResendRequestTests` 制造输入序号缺口，验证 `BeginSeqNo=2, EndSeqNo=0` 的 ResendRequest，并在补齐
  序号 2 后继续处理已排队的序号 3。
- `DirectSocketLogonTimeoutTests` 验证建立 TCP 连接但不发送 Logon 时会关闭连接。
- `DirectSocketGracefulStopTests` 使用真正的 acceptor 网络线程调用 `stop(false)`，验证 Logout 发到线上、收到对端
  Logout 回应后在限定时间内正常停止。

构建和验证命令：

下面沿用已有的 `build-direct-stage3` 目录名以避免重新生成一套大型构建目录。构建目录名只是 CMake 缓存名称；
源码更新后执行增量 build，产出的二进制已经包含阶段四。

```bash
cmake --build build-direct-stage3 --target ut fix_parse_benchmark_target --parallel 2

./build-direct-stage3/out/ut DirectSocketWriteTests
./build-direct-stage3/out/ut DirectSocketWriteBackpressureTests
./build-direct-stage3/out/ut DirectSocketSessionTimerTests
./build-direct-stage3/out/ut DirectSocketResendRequestTests
./build-direct-stage3/out/ut DirectSocketLogonTimeoutTests
./build-direct-stage3/out/ut DirectSocketGracefulStopTests
./build-direct-stage3/out/ut "Direct*"
./build-direct-stage3/out/ut --quickfix-spec-path spec
```

2026-07-22 的阶段四验收结果：全部 `Direct*` 测试通过，共 6034 个断言、9 个测试用例；开启 direct 的完整
单元测试通过，共 7965 个断言、62 个用例；关闭 `QUICKFIX_DIRECT_READ_POLL` 的 baseline 完整测试通过，共
1990 个断言、53 个用例。相同 direct-capable 二进制下，blocking、poll0 和 direct 各发送 10000 条 raw
NewOrderSingle，三组 `received` 均为 10000。

当前实际配置设计：

- `SocketBusyPollMode=blocking`：原始阻塞 `poll`。
- `SocketBusyPollMode=poll0`：原有 `poll(..., 0)` busy-poll。
- `SocketBusyPollMode=direct`：阶段四 direct scan、背压安全发送和会话定时路径。
- `SocketBusyPollCpu=N`：为 `poll0` 或 `direct` 的 acceptor 网络线程绑核。

旧配置 `SocketBusyPoll=Y` 继续表示 `poll0`，用于兼容已有实验。它不能和 `SocketBusyPollMode` 同时设置。
read-drain、direct scan budget、空闲退避和周期性 readiness 检查仍未实现。

### 3.14 推荐实验顺序

建议按下面顺序比较，避免一次叠太多变量：

1. 原始阻塞 `poll`，无 SIMD，无 fixed-layout。
2. 第一阶段 busy-poll，只改等待方式。
3. 第一阶段 busy-poll + 绑核。
4. SIMD stream parser。
5. fixed-layout parser。
6. busy-poll + SIMD stream parser。
7. busy-poll + SIMD stream parser + fixed-layout parser。
8. direct recv scan 阶段二：单连接直读。
9. direct recv scan 阶段三：多连接扫描与写队列。
10. direct recv scan 阶段四：背压安全发送、会话定时器与正常停止。
11. 未来 read-drain、scan budget 和空闲退避。

每一组最好分别测：

- `new-order-single`
- `order-cancel-request`
- `market-data-snapshot`

普通 QuickFIX baseline 如果要贴近原始完整校验路径，应使用 `--validate=yes`。fixed-layout 实验可以不加 `--validate=yes`，因为 fixed-layout 的目标就是跳过普通校验和字段扫描成本。

### 3.15 benchmark 使用方式

编译 busy-poll-capable build：

```bash
cmake -S . -B build-bench-busy-poll \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=ON \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=ON \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=ON \
  -DQUICKFIX_BUSY_POLL=ON

cmake --build build-bench-busy-poll --target fix_parse_benchmark_target -j 2
```

运行时关闭 busy-poll，作为同一二进制下的阻塞 poll baseline：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=1000000 --port=54381
```

启用 busy-poll：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=1000000 --port=54382 --busy-poll
```

启用 busy-poll 并绑定 acceptor 线程：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=1000000 --port=54383 --busy-poll --busy-poll-cpu=2
```

和 fixed-layout 组合：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --fixed-layout --messages=1000000 --port=54384 --busy-poll --busy-poll-cpu=2
```

行情普通解析 baseline 建议显式打开 validate：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --messages=1000000 --port=54385 --validate=yes
```

行情 fixed-layout + busy-poll：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --fixed-layout --messages=1000000 --port=54386 --busy-poll --busy-poll-cpu=2
```

### 3.16 当前结论

`poll0` busy-poll 已经提供了一个可开关的网络等待实验路径：

- 默认不影响普通阻塞 `poll` baseline。
- 打开后通过 `poll(..., 0)` 让 acceptor 线程持续轮询。
- 可用 `--busy-poll-cpu=N` 把 acceptor 网络线程绑定到指定 CPU。
- 会明显增加 CPU 占用，但不一定保证吞吐提升。

direct-read 阶段四也已完成：它绕过每轮 `poll()`，并已验证多 Session 公平扫描、真实发送背压、部分写入、
`EAGAIN` 续发、Heartbeat、TestRequest、ResendRequest、Logout、Logon/Logout 超时和正常停止。它仍是低连接数
实验路径；大量空闲连接会产生大量 read `EAGAIN`。后续应分别评估 read-drain、scan budget、周期性
readiness 检查和空闲退避。
