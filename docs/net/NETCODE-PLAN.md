# NETCODE-PLAN — 第 12 步「多人游戏 / 网络栈」落码计划（2026-11-13）

> **状态：开工闸已解除**。六项架构判定 Q-N1~Q-N6 全部拍板（结论权威落点 `NETCODE-OPTIONS.md` §6 + `GAME-DEV-ORDER.md` 头部），本计划是拍板结论的**任务分解**，不引入新架构判定；执行中出现新的「待拍板」级问题须停下补拍板，不许顺手写死。
> **证据基础**：`NETCODE-OPTIONS.md` §1 三处定向实读（Carrot `network/` 五文件 + 同步层 grep 零命中；bevy 官方无网络层 crate，分层参照 `bevy_replicon` / `lightyear`；UE 只读 `NetDriver.h` / `RepLayout.h` / `Iris/ReplicationSystem.h`）。
> **拍板速览**：多人进路线（Q-N1）｜服务器权威 + 快照插值（Q-N2）｜ENet vendored（Q-N3）｜`msgpack_min` 保留（编辑器 RPC）+ bitsery vendored（Q-N4）｜最小闭环 = 只同步载具 transform + 玩家加入退出、不纳管 editor RPC（Q-N5）｜`RPC-SPEC.md` 勘误已执行（Q-N6）。

---

## 0. 全局不变量（每个里程碑都适用）

| 项 | 口径 |
|---|---|
| 第三方库 | 只许 ENet（M-NET0）与 bitsery（M-NET1）两个新 vendored；各带 `thirdparty/<lib>/VENDOR.md`（版本 / 上游 / 许可证 / C++ 标准 / 入库子集 / 风险核查）+ LICENSE 原文入库；业务代码经 `nl/net/` 薄封装隔离，不暴露 ENet / bitsery 类型 |
| 构建闸 | 零构建期新依赖：纯源码直接进 CMake 构建，MSVC 14.38 / C++17 可编；不引新运行时 DLL |
| 线协议 | 快照 = bitsery 位打包 + 定长量化（M-NET1 定字段）；控制消息（加入 / 退出 / 心跳类）走 ENet 可靠通道；快照流走 ENet 不可靠通道（丢旧保新） |
| 回滚 | `P8-GAMELOOP-PLAN.md` §4「回滚 netcode 不在范围」**继续有效**，本计划无任何回滚内容 |
| editor RPC | `editor_bridge/` 与 `RPC-SPEC.md` 轨道**零改动**；`msgpack_min.hpp` 留在原处，不搬移、不复用其 Value 树做快照 |
| kStage | 不 bump；渲染可见验收走 `example/<版本>/` 新卡 + `run.bat verify` + `regression.py --only <版本>` 双机械校验（M9.0 口径：`server.py --check` 只证明旧阶段无回归） |
| tag | `net-m0` ~ `net-m3` / `net-done`（对齐 `nav-m0`~`nav-done` 先例）；tag 在验收通过之后才打 |
| 轨道归属 | 对齐 `feat/pathfinding`（nav 轨道）先例：注册并行轨道 `net`（`feat/net` 分支 + wt junction，PowerShell 原生 API 建/删）；每个对话开场 `wt_pool.py claim --track net --milestone M-NETx`，收尾对称 release |

---

## 1. 里程碑分解

### M-NET0 — 传输层：ENet vendored + `nl/net/` 薄封装 + echo tool 卡

- **目标**：双进程可靠建链、双向回显，亲眼可见。
- **任务**：
  1. `thirdparty/enet/`：vendored ENet（上游 lsalzman/enet，目标 v2.x 最新 tag；许可 MIT）、`VENDOR.md`、LICENSE；CMake 纯源码接入（Win32 winsock2 路径，零新外部依赖）。
  2. `nl/net/`：`Endpoint` / `TransportHost` / `TransportPeer` 薄封装（建链 / 断链事件、可靠 / 不可靠双通道发送接收、tick 驱动 `host_service`）；**不含任何游戏语义**。
  3. 工具 exe 子命令 `net-echo-server` / `net-echo-client`（`--host` / `--port` / `--packets` / `--rate` / `--loss%`；echo 延迟统计 stdout）。
  4. 展示台 `kind:"tool"` 用例三条起步：usage（`--help`）/ 正向（双进程 echo N 包零丢失）/ 负向（坏 endpoint 报错退出码 1）；`expectLog` 从实测 log 复制，禁手写。
- **验收判据**：tool 卡三条全绿 + 丢包注入（10%）下 echo 延迟曲线证据图（从工具真实输出生成，`preview` 静态图）+ ctest 双后端重新 configure 后全绿（`--repeat until-pass:3`）+ `regression.py --only` 挑 2~3 条既有用例无回归。

### M-NET1 — 序列化层：bitsery vendored + 载具 transform 快照编解码

- **目标**：transform 快照位打包编码 / 解码逐字节稳定，载荷实测落档。
- **任务**：
  1. `thirdparty/bitsery/`：vendored（上游 fraillt/bitsery v5.2.5，MIT，header-only）、`VENDOR.md`、LICENSE。
  2. `nl/net/`：快照结构体（server tick、entityId、位置 / 朝向 / 线速度，**量化字段宽度 M-NET1 任务书内定死**：位置 mm 精度 / 朝向四元数压缩或欧拉有限域 / 速度 cm/s）+ bitsery 位打包 encode / decode + 往返字节级测试。
  3. 载荷实测：固定快照数下平均字节数 / 编解码耗时，写进工具指南归档（README 自报基准不作数）。
- **验收判据**：encode→decode 往返 bytes 级一致（ctest）+ 量化误差 ≤ 声明精度（数值断言）+ tool 卡（usage / 正向 / 负向：截断流报错）+ 实测表归档。

### M-NET2 — 服务器权威最小闭环：两实例同场景位置一致

- **目标**：一台 server + 至少一台 client 连入同场景，client 看到彼此载具位置插值跟随，亲眼可见截图。
- **任务**：
  1. viewer 进程新增运行模式 `--net-server <port>` / `--net-client <host:port>[,name]`：server = 60Hz GameClock 驱动 Jolt 载具权威模拟，快照流按 tick 发不可靠通道；client = 本地载具输入上送 + 远端载具快照收包入插值缓冲，`alpha` 帧间插值渲染（复用 M-GL3 机制）。
  2. 加入 / 退出：可靠通道握手（client hello → server 欢迎含当前 tick 与实体分配）+ 断链清理（实体回收）。
  3. HUD 状态行：连接态 / RTT / 快照 tick 差（ASCII，高 ≤560 面板约定不变）。
  4. `example/net-m2/` 版本卡：双进程脚本（server + client 同场景）基线截图 + `run.bat`（含 `verify`）。
- **验收判据**：数值锚 = client 侧远端载具插值位置与 server 权威轨迹同 tick 偏差 ≤ 插值缓冲一个快照间隔（工具输出断言）；截图 = 双实例同场景可见一致画面；`regression.py --only net-m2` 双后端过 + 既有 107 张卡抽 2~3 条无回归。

### M-NET3 — 打磨与收官：`net-done`

- **目标**：断线 / 重连 / 参数打磨 + 全量总闸收官。
- **任务**：插值缓冲参数（缓冲长度）外置 CLI；异常路径（server 退出 / client 断线后本地继续跑不崩）；`docs/net/NETCODE-TOOLS-GUIDE.md` 归档；`MILESTONES.md` 四栏；tag `net-m3` + `net-done` + `milestone/net-done` 分支（大版本收官纪律 5b）。
- **验收判据**：全量回归双后端一次（tag 收口口径）+ `server.py --check` + 收口第一屏四项自查 + 三要素表格。

---

## 2. 里程碑间依赖与排期

```
M-NET0（ENet + echo）──→ M-NET1（bitsery + 快照编解码）──→ M-NET2（最小闭环）──→ M-NET3（收官）
```

- 单人串行；M-NET0 / M-NET1 与主线渲染零耦合，可与其它并行轨道穿插，但同一 wt 内不并行。
- 每个里程碑独立可验收、独立 tag；收口第一屏 = 四项自查 + 三要素表格。

## 3. 冻结面（PLAN 落定后仍不许动的）

- 除 ENet / bitsery 外的任何第三方库引入；
- `editor_bridge/`、`RPC-SPEC.md`（Q-N6 勘误已完成，此后零改动）；
- `P8-GAMELOOP-PLAN.md` §4 回滚排除条款；
- 既有 107 张 example 卡（M-NET2 新卡按拍板新增，不动旧卡）；
- 量化字段 / 线协议格式在 M-NET1 任务书定死后，M-NET2 起改格式须重拍 + 重拍基线。

## 4. 变更记录

- 2026-11-13：新建。依据 Q-N1~Q-N6 六项拍板（见 `NETCODE-OPTIONS.md`）分解为 M-NET0~M-NET3；零代码改动，落码自 M-NET0 claim 后开工。
