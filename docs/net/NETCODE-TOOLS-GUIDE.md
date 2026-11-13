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

## 3. M-NET2（服务器权威最小闭环）— 已落码（2026-11-13）

### 3.1 控制消息线格式（`core/include/nl/net/Sync.hpp` 头注释为契约，改动须重拍）

控制消息全部 pack(1) POD，**统一线格式头恒在 offset 0..8**：`magic(4) + proto(4) + type(1)`，单点校验 `ctrlHeadOk`（n≥9、magic/proto 匹配、d[8] = 期望 type）。`kSessionMagic = 0x4E4C534E`（"NLSN"）、`kSessionProto = 1`；每组消息配 `static_assert(sizeof) == 字节数` 锁尺寸。

| 消息 | type | 字节数 | 方向 | 载荷 | 通道 |
|---|---|---|---|---|---|
| CtrlHello | 1 | 25 | C→S | name(16，ASCII 截断收尾) | 可靠 |
| CtrlWelcome | 2 | 16 | S→C | serverTick(4) + entityId(2) + 分配信息 | 可靠 |
| CtrlInput | 3 | 28 | C→S | forward/right/brake/handBrake（4×float）；解码含 `std::isfinite` NaN 拒绝 | 可靠 |
| CtrlEntityGone | 4 | 14 | S→C | entityId(2) | 可靠 |
| CtrlLeave | 5 | 9 | C→S | （仅头） | 可靠 |

快照流（`VehicleSnapshot`，M-NET1 线格式 31 B）走不可靠通道丢旧保新，与控制消息通道分离。

### 3.2 同步层与插值（`Sync.cpp`，零第三方类型）

- **InterpBuffer**（kCapacity=32 环）：tick 单调 push（丢旧保新）；`eval(delayTicks, out, evalTickOut, extraTicks=0)` 在相邻快照对间 lerp / nlerp（dot<0 取反短弧，退化取端点 a）；target < oldest 返回 false（未预热）。`extraTicks` 为帧间相位分数（viewer 用最新快照到达后实钟经过折算，上限 0.95），确定性单测传 0。
- **ServerSession**（kMaxEntities=16 槽位）：`claimLocalEntity` / `pump(joined,left)`（Hello→分配实体+**建输入槽**+回 Welcome；Input→缓存；Leave / PeerDisconnected→回收+广播 EntityGone）/ `inputOf` / `broadcastSnapshot` / `notifyEntityGone`。
- **ClientSession**：`beginConnect` / `pump` / `sendInput` / `bufferOf` / `entityAt` / `forgetEntity` / `welcomed` / `rttMs`；`disconnect` 拆链并**复位会话态**（welcomed_ / localEntityId_ / bufs_）。
- viewer 口径：`--net-server <port>` = 60Hz GameClock 驱动 Jolt 权威模拟，快照按 tick 发不可靠通道；`--net-client <host:port>[,name]` = 输入上送 + 快照入插值缓冲，`kInterpDelayTicks=2.0`，相位 `extra=min((nowMs−arrival)/kNetTickMs, 0.95)`；互斥与参数校验前置（exit 2），NetGuard RAII 一比一。

### 3.3 子命令（`net-tools`，M-NET2 新增两条）

| 子命令 | 语义 | 退出码 |
|---|---|---|
| `session-loopback [--port P] [--ticks N]` | 单进程全闭环：client beginConnect→泵→sendInput→server.pump（Welcome/实体分配）→广播快照→client.pump→eval 逐样本误差断言→disconnect 核对 left + 会话态复位。PASS 锚 `session-loopback : PASS` | 0=PASS / 1=FAIL / 2=超时 |
| `verify-m2 --auth FILE --interp FILE [--min-samples N]` | server log 的 `auth tick=.. px=..` 行与 client log 的 `interp tick=.. px=..` 行按 tick 配对；阈值 = **自标定** `|a(t+1)−a(t)| + 0.005`（一个快照间隔位移 + 量化余量），matched<minSamples 记违约。`--selftest` = 精确 lerp 必过 + 注入 +0.5m 偏移样本必炸 | 0=PASS / 1=FAIL / 2=用法 |

ctest（双后端 `--repeat until-pass:3`）：`net-m2-session`（PASS_REGULAR_EXPRESSION `session-loopback : PASS`）/ `net-m2-verify` / `net-m2-verify-negative`（WILL_FAIL）。展示台第 110 张版本卡 `net-m2`（4 用例 kind:tool，`exe` 为 `{"dx12":..., "vulkan":...}` 字典）。

### 3.4 run.bat 六段（`example/net-m2/run.bat`，ALL GREEN 基线 2026-11-13）

1. `session-loopback`（300 ticks）→ 2. `verify-m2 --selftest` → 3. **双进程**：server `start /b ""` 先行 + `ping` 延后 2s 起 client（防时序竞争），server 800 帧 / client 900 帧，`--yaw 0 --pitch 35 --dist 26 --hud`，findstr 锚 `listening` / `joined` / ghost slot / `interp tick` → 4. `verify-m2 --auth server.log --interp client.log --min-samples 200` → 5. 负向：interp 错喂 server.log 必 FAIL exit 1 → 6. 轮询 server `shutdown ok` + 注记 `entity 2 left`（断链实体回收）→ ALL GREEN。双实例截图 `server.png` / `client.png` 各含两台黄色线框车体——**PNG evidence-only 不做 MD5 基线**（双进程包到达含实时时序，不可复现字节级）。

### 3.5 实测表（2026-11-13，DX12）

| 项 | 实测值 | 判定 |
|---|---|---|
| session-loopback 300 ticks | 逐样本 maxErr≈0（整点采样）maxBound=0.1853 | 插值不越界 ✓ |
| verify-m2 双进程 | matched=1458 viol=0 maxErr=0.0008 maxRatio=0.140 | ≤ 一个快照间隔 ✓ |
| client 行驶段位移 | interp tick 225→443 px −5.98→−6.87 | 有位移非冻结 ✓ |
| 断链回收 | client 退出后 server 广播 EntityGone + removeVehicle（tombstone 保槽位号） | `entity 2 left` ✓ |
| 零回归 | ctest net 族 12/12 双后端全绿；regression net-m2 4/4 + 抽查 drive-m3 6/6 / drive-m2 5/5 保形 | ✓ |

### 3.6 踩坑留档（M-NET2 实测）

1. **控制消息线格式头错位**：首版 CtrlInput/CtrlEntityGone 漏 proto 字段（type 落 offset 4），而 `ctrlHeadOk` 校验 d[8] → **所有 Input 包被静默拒收**（loopback `input=BAD` 探针定位）。统一全部消息为 magic+proto+type 头 + static_assert 锁尺寸后闭环。
2. **Hello 建实体必须同步建输入槽**：只填 peers_[slot] 不填 inputs_ 槽 → hasInput=0，server 拿不到 client 输入。Hello 处理补 `inputs_[slot] = {ent, InputState{}}`。
3. **验收探针禁止一次性锁存**：`inputOk` 首次比较发生在 welcome 前（输入丢弃期）→ 改为每迭代持续检查直到通过。同理 `inputOk 错误锁存` 是 loopback 断言设计纪律。
4. **插值需要真 alpha 相位**：target = latest − 2.0 恒整数 tick，eval 只在整点采样 → `eval` 加 `extraTicks` 相位参数；viewer 按最新快照到达后实钟经过折算（上限 0.95 防越过最新样本）。
5. **数值锚阈值自标定**：不从速度假设推导，取 auth 轨迹相邻位移 `|a(t+1)−a(t)| + 0.005`——免速度模型、抗偶发丢包拉长段；`minSamples` 防证据不足假 PASS。
6. **双进程握手时序竞争**：server 帧数跑完时 client 才启动 → 必然 0 interp。ENet CONNECT 重试约 1s 节奏赢不了无节流帧率（~350fps 下 210 tick 行驶窗口仅 0.6s）→ run.bat `start /b` + `ping` 延后 2s；确定性验收不依赖双进程实时时序，走 loopback + selftest 工具锚。
7. **disconnect 必须复位会话态**：只拆 host 不清 welcomed_/localEntityId_/bufs_ → 复用会话对象行为残留。

## 4. M-NET3（打磨收官，tag `net-m3` + `net-done`）— 已落码（2026-11-13）

### 4.1 变更面

| 项 | 语义 | 位置 |
|---|---|---|
| `--interp-delay <ticks>` | client 快照插值回看深度外置 CLI（默认 2.0，范围 1.0..8.0，越界 exit 2）；替换原 `kInterpDelayTicks` 常量，eval 相位 / HUD delay 行同源消费。缓冲容量 kCapacity=32 为编译期环，属实现细节不外置 | viewer `Options` / `parseArgs` / runNormal 前置校验 |
| server 优雅关服 | `NetServer::Impl::destroy`（stop/析构路径）对每个活跃 peer `enet_peer_disconnect` + `enet_host_flush` 再 destroy——对端**立即**收到断链事件，不再依赖 ENet 默认超时（~30s） | `core/src/net/Net.cpp` |
| client 断链边沿 | viewer client 每帧跟踪 `welcomed` true→false 边沿 = server 断链 → 回收全部远端幽灵（removeVehicle）+ 日志锚 `client: server disconnected, ghosts removed, continuing locally`；client 零本地模拟，继续跑不崩 | viewer main.cpp |
| HUD disconnected 态 | `welcomed 过但链路已断` = `disconnected`（区别于从未连上的 `connecting`） | viewer main.cpp HUD Net Status |
| `session-loopback` 扩被动断链段 | 独立新会话套：泵到 welcomed → `server.stop()`（优雅关服）→ client 循环 pump → 断言 `welcomed==false && connected==false && entityCount==0 && localEntityId==0`，输出锚 `session      : passiveClose=clean`，纳入 PASS 断言 | net-tools `cmdSessionLoopback` 尾段 |

### 4.2 实测表（2026-11-13，DX12）

| 项 | 实测值 | 判定 |
|---|---|---|
| loopback 被动断链段 | `passiveClose=clean`（300 ticks 全 PASS） | 优雅关服 → client 立即复位 ✓ |
| `--interp-delay` 越界 | 0.5 / 9 → `exit 2` + 明确错误行 | 用法错误不静默降级 ✓ |
| 双进程断链（真实时序） | server 先退 → client.log 出现 `client: server disconnected` 锚，client 跑完全部帧 exit 0 | 断链后本地继续跑不崩 ✓ |
| run.bat 全段 | ALL GREEN exit 0（matched=213 viol=0 maxErr=0.0007 maxRatio=0.087） | ✓ |
| ctest net 族 | 12/12 双后端 `--repeat until-pass:3` 全绿 | ✓ |

### 4.3 收官纪律（tag `net-m3` + `net-done`）

- 全量 regression 双后端各跑一次（tag 收口口径，非 `--only` 抽查）；`tools/showcase/server.py --check` 过。
- annotated tag `net-m3`（本里程碑）+ `net-done`（第 12 步网络栈大收官）+ `milestone/net-done` 分支（大版本收官纪律 5b）。
- 验收口径注意：**双进程退出顺序是实时相关的**，viewer 层断链锚（`client: server disconnected`）在 run.bat 里是软注记（命中才打印，不 FAIL）；确定性锚 = loopback `passiveClose=clean`（Sync 层全语义，ctest 锁定）。

### 4.4 踩坑留档（M-NET3 实测）

1. **`enet_host_destroy` 不通知 peer**：server 退出直拆 socket，client 端要等 ENet 默认超时（~30s）才感知断链——「server 退出 client 不崩」缺的不是崩溃处理而是**感知时延**。优雅关服（逐 peer disconnect + flush 后 destroy）一行不落，client 断链边沿永远测不出即时语义。
2. **被动断链验证不能依赖双进程时序**：server/client 帧数都按无节流帧率折算成秒级，退出顺序实时相关 → run.bat 里只做软注记；确定性验证放在 loopback 内独立会话套（server.stop → client.pump 收断链）。
3. **ENet client 收断链后 `connected()` 自动翻 false**（peer state = DISCONNECTED），HUD 三态不够用——「welcomed 过但断开」的 `disconnected` 态需要 viewer 侧自行跟踪边沿，Sync 层无需加状态位。
