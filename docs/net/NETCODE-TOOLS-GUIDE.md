# NETCODE-TOOLS-GUIDE — 网络栈工具归档（docs/net/，net 轨道）

> 工具 = `net-tools`（`build/{dx12,vulkan}/net-tools.exe`，纯 CPU 零图形 API，双构建目录同一二进制口径）。
> 退出码：**0 = PASS / 1 = 错误（建链失败等）/ 2 = 用法错误或超时**（对齐 nav-tools）。
> 展示台版本卡：`example/cases.json` `net-m0`（6 用例 `kind:"tool"`）；`example/net-m0/run.bat` 为 run.bat 交代。

## 1. M-NET0 — 传输层（2026-11-13，tag `net-m0`）

### 1.1 子命令

| 命令 | 形态 | 退出码 | 说明 |
|---|---|---|---|
| `echo` | **单进程回环**（同进程 server+client 双 host，单线程交替泵） | 0=OK / 1=init或beginConnect失败 / 2=超时 | `--port`(默认29191) `--reliable`(32) `--unreliable`(64) `--payload`(64) `--timeout-ms`(10000)；可靠+不可靠双通道全量回显断言，rtt 行为时变字段不作锚 |
| `echo-server <port>` | 双进程服务端 | 0=服务满 / 1=start失败 / 2=超时 | `--packets`(64) `--max-clients`(4) `--linger-ms`(0) `--timeout-ms`(30000)；linger>0 时服务满后继续存活 N ms（丢包 demo 收尾用） |
| `echo-client <host> <port>` | 双进程客户端 | 0=全回显 / 1=连接失败或断链 / 2=丢包或超时 | `--packets`(32) `--rate`(200) `--loss`(0) `--payload`(64) `--timeout-ms`(10000)；`--loss P` 模拟入站丢包（`mt19937` 固定种子 1234，确定性序列） |
| `selftest` | 机械断言 | 0=PASS / 1=FAIL | 16 可靠 + 32 不可靠小参数回环 + 全量回显断言 |
| `version` | 溯源锚 | 0 | 输出 `enet : 1.3.18`（ENET_VERSION 宏拼接，防库漂移） |

### 1.2 ctest（双后端各 5 条，`--repeat until-pass:3` 口径）

| 用例 | 断言 |
|---|---|
| `net-m0-echo` | `echo        : OK`（32/32 + 64/64 全量回显） |
| `net-m0-selftest` | `selftest net: PASS` |
| `net-m0-version` | `enet        : 1.3.18` |
| `net-m0-usage-negative` | 未知子命令 exit 2（`WILL_FAIL TRUE`） |
| `net-m0-connect-refused` | 死端口握手 exit 1（仅 `WILL_FAIL TRUE`；FAILED 文本锚由 run.bat findstr 兜底） |

> ⚠️ **WILL_FAIL + PASS_REGULAR_EXPRESSION 组合语义是「正则不得命中」**（ctest 实测反向），负向例只留 WILL_FAIL——口径对齐 anim-tools。

### 1.3 run.bat 段落（`example/net-m0/run.bat`，ALL GREEN 基线 2026-11-13）

1. version 锚 → 2. usage（exit 2）→ 3. 单进程回环（32/32 + 64/64 + `echo OK`）→ 4. selftest → 5. **双进程零丢失**（server 29192 + client 32 包 `sent=32 recv=32 loss=0.0%` + 轮询 server `served=32`）→ 6. **双进程 30% 丢包注入**（`--loss 30` 种子 1234 实测 recv=22 loss=31.2%，exit 2 为预期）→ 7. 死端口负向（exit 1 + `connect : FAILED`）。

### 1.4 踩坑留档（M-NET0 实测）

1. **ENet 服务器只在 `enet_host_service` 时处理握手包**：单进程回环里 `connect()` 阻塞等 CONNECT 期间同线程 server 没在泵 → 必超时。回环路径必须 `beginConnect()` + 双 host 交替泵（`connect()` 仍可用于双进程：对端在自己进程里泵）。
2. **`WILL_FAIL TRUE` 与 `PASS_REGULAR_EXPRESSION` 不可同用**：组合语义 = 正则不得命中（见 §1.2 注）。
3. **沙箱回收 `start` 派生子进程**：`start "title" /min` 开新窗的写法在沙箱会话必死（server.log 空、client 连不上）；`start /b ""` 直起 exe 子进程存活且继承重定向句柄。正常 cmd 环境两种都可用，run.bat 取 /b 兼容两者。
4. **run.bat ASCII only 重申**：rem 注释里的 em dash（—）在 GBK 控制台被 UTF-8 双字节拆碎，后续行解析错位（`'m' 不是内部或外部命令`），非 ASCII 一律用 ASCII 连字符。
5. **入站丢包无重发**：模拟丢包丢弃的回显没有第二次机会，server 必须撑到 client 先到 deadline 打出统计（`--linger-ms` 的由来）。

## 2. M-NET1（序列化层，bitsery）— 已落码（2026-11-13）

### 2.1 线格式（`core/include/nl/net/Snapshot.hpp` 头注释为契约，改动须重拍）

| 字段 | 量化 | 位宽 | 声明精度 / 范围 |
|---|---|---|---|
| serverTick | uint32 原值 | 32 | 1 tick |
| entityId | uint16 原值 | 16 | 最小闭环 ≤65535 实体 |
| position | int32 毫米逐轴，饱和钳制 | 3×32=96 | 1 mm；±2147483.647 m |
| orientation | 四元数 smallest-three（2b 索引 + 1b 符号 + 3×16b） | 51 | 分量 ≤1.08e-5；**端到端角误差 ≤0.1 度** |
| velocity | int16 cm/s 逐轴，饱和钳制 | 3×16=48 | 1 cm/s；±327.67 m/s |

合计 **243 bits → 31 bytes**（naive float 全量 46 B，**省 32.6%**）。位序 = bitsery 位打包（LSB-first，双端一致）。

### 2.2 子命令（`net-tools`，M-NET1 新增三条）

| 子命令 | 语义 | 退出码 |
|---|---|---|
| `snapshot-selftest` | 256 样本（seed 1234）往返 bytes 级一致 + 精度断言 + 恒等/负 largest/饱和边界 + 截断/尾部垃圾/脏残位三类负向 | 0=PASS / 1=FAIL |
| `snapshot-bench [--count N]` | 确定性载荷实测：avgBytes / enc+dec ns/op / 量化最大误差 / 逐字段位宽表 / naive 对照 | 0 / 1 / 2 |
| `snapshot-decode <hex>` | hex 线流解码（负向工具卡：短流显式 FAILED） | 0 / 1 / 2 |

ctest（双后端 `--repeat until-pass:3`）：`net-m1-selftest` / `net-m1-bench` / `net-m1-decode-negative`（WILL_FAIL）。

### 2.3 实测表（2026-11-13，DX12，seed 1234 count=20000）

| 项 | 实测值 | 判定 |
|---|---|---|
| 线格式字节数 | 31.00 B（243 bits 对齐） | 契约值 31 ✓ |
| naive float 对照 | 46 B → **省 32.6%** | PLAN 验收判据 ✓ |
| encode | ~250–400 ns/op | 远小于 60Hz 帧预算 16.6ms ✓ |
| decode | ~150–230 ns/op | 同上 ✓ |
| 位置最大误差 | 0.000610 m | ≤ 声明 1 mm ✓（半步 0.5mm + float 存储噪声） |
| 朝向最大角误差 | 0.0685 度 | ≤ 声明 0.1 度 ✓（见下注） |
| 速度最大误差 | 0.005001 m/s | ≤ 声明 1 cm/s ✓（半步 0.5cm/s + float 噪声） |

> **朝向误差注**：线格式分量量化贡献仅 ~0.002 度（2·√3·(1/√2)/65535，探针逐分量核对：最坏样本 qx 6.2e-6 / qz 9.4e-6 / largest 4.7e-7，全部 ≤1.08e-5）；端到端 ~0.07 度由**输入四元数 float 归一化范数亏差（~2e-7）被 smallest-three 重建强制单位范数后经 2·acos 的 sqrt 放大**造成——与 UE FQuat 网络压缩角误差（~0.1 度量级）同级。M-NET2 服务器权威侧输入同源（Jolt float 四元数），口径一致。

### 2.4 踩坑留档（M-NET1 实测）

1. **bitsery `OutputBufferAdapter` 的 `vector::size()` 不可信**：可增长 buffer 会被增长策略撑到超过实际写入字节数；编码字节数必须取 `writtenBytesCount()`（首版 encode size=0 即此根因，直拉实锤）。
2. **`writeBits`/`readBits` 仅收无符号整型**（static_assert + assert）：有符号值须按二补码位型 cast 后写（pos mm / vel cm/s 都是这么做的）。
3. **对齐残位是校验面**：bitsery `align()` 在 `CheckDataErrors` 开启时对非零残位报 `InvalidData`——解码侧显式利用（脏残位 = 拒收），selftest 负向 3 锚死该行为。
4. **函数参数 `float q[4]` 退化为指针**：range-for 不成立（C2672），用索引循环。
5. **阈值 = 声明精度而非半步**：半步断言（0.5mm/0.5cm/s）会被 float 存储噪声击穿（实测 0.00061 m / 0.005001 m/s）；诚实口径 = 网格步长（1mm/1cm/s）+ 误差来源拆解写进注释。

## 3. M-NET2（服务器权威最小闭环）— 未开工

## 4. M-NET3（打磨收官）— 未开工
