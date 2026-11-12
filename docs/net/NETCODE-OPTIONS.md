# 多人游戏网络栈选型决策材料（传输 / 序列化 / 同步三层，2026-11-13）

> **状态：Q-N1 ~ Q-N6 全部已拍板（2026-11-13）。** 本文件是「摊方案」的**决策证据**，不是计划；结论以用户拍板后写回 `GAME-DEV-ORDER.md` / `docs/net/NETCODE-PLAN.md` 的开工拍板记录为准。格式参照 `docs/s9/Q4-GBUFFER-OPTIONS.md`（其 §6 建议为拍板前撰写，保留供追溯，不代表后续落码口径）。
> **触发对话**：2026-11-13「可以考虑后面往多人游戏方向开发 接入游戏常用的 kcp 协议和 msgpack 吧」→「顺便看下有没有更好的开源解决方案」→「同步层也算上吧」→「之前没打算做 现在想做了 加到计划里」（范围从 L1+L2 扩为 **L0~L4 全覆盖**，随后 Q-N1 拍板进路线）。
> **性质**：方向性探讨 → Q-N1 拍板后已写回台账。**零代码改动**；文档变动仅限本文件 + `GAME-DEV-ORDER.md` + `NON-RENDER-ROADMAP.md`（拍板写回与旧口径勘误）。未建 track、未 claim、未打 tag、未跑回归。
> **Q-N1 拍板内容**：用户原话「之前没打算做 现在想做了 加到计划里」→ **多人游戏正式进路线**；已写回 `GAME-DEV-ORDER.md` 第 12 步，并同步勘误 `NON-RENDER-ROADMAP.md` L38 / L47 / L67 的 ★★ 慎入旧定档。**范围 / 技术选型未拍板**：Q-N2 同步模型、Q-N3 传输层、Q-N4 序列化、Q-N5 范围边界、Q-N6 勘误仍待拍板，按 `AGENTS.md` 开工闸**不许落码、不建 PLAN、不建 track**；§5 建议保留原样供追溯。

---

## 0. 冻结面（拍板前不许写第一行）

| 项 | 状态 |
|---|---|
| `nl/net/` 任何头文件 / 源文件 / CMake entry / 构建目标 | **冻结** |
| `thirdparty/` 新增任何网络库（KCP / ENet / GNS / yojimbo / SLikeNet / msgpack-c 等）与对应 `VENDOR.md` | **冻结**（拍板后按选型硬闸逐项评审） |
| 线协议字段、快照结构、tick 号语义、新增 CB / 资产格式 | **冻结** |
| `GAME-DEV-ORDER.md` 台账、`NON-RENDER-ROADMAP.md` 定档 | **Q-N1 已拍板，2026-11-13 已按拍板更新**（第 12 步 + L38/L47/L67 勘误） |
| `P8-GAMELOOP-PLAN.md` §4「回滚 netcode 不在范围」 | **冻结**（仅当选「确定性回滚」时须单独解禁） |
| 在注释 / README / commit message / CMake 里顺手把网络形态写死 | **冻结** |
| 本文件 + `GAME-DEV-ORDER.md` + `NON-RENDER-ROADMAP.md`（Q-N1 拍板写回与勘误） | **本轮仅有的三处文档变动** |

> 机器核查：本轮全程只读工具 + `git status --porcelain`；工作区 20 余处 `M` / `??` 均为既有改动（M12.2 之前的状态），与本轮无关；本轮新增 / 修改的 3 份文档见上表。

---

## 1. 三处定向实读证据（本轮，带路径）

### 1.1 Carrot：只到连接 / 发包层，**同步层零命中**

- 实读（上一轮）：`engine/engine/network/NetworkInterface.h`、`Packet.hpp`、`client/Client.h`、`server/Server.h`、`ConnectionState.h`。
- 本轮 grep `snapshot|replic|interpol|predict|rollback|stateSync|tick` 于 `engine/engine/network/` → **No matches found**。
- `docs/NON-RENDER-ROADMAP.md` L67 自评 `network` 888 行「✅ 本表『网络同步』条目（★★ 慎入）」——**该 ★★ 定档已于 2026-11-13 Q-N1 拍板作废，此处仅引用历史原文作证据**。
- **定性**：Carrot 只能作**连接层节奏参照**，同步层在本仓库无任何可参考代码。

### 1.2 bevy：官方引擎自带**零网络层**，复制层全在第三方生态

- 实查 `E:\UE_STUDY_2026\bevy\crates\`：`grep -iE "net|renet|replic|multi|lightyear"` → 仅 `bevy_world_serialization`；Glob `crates/bevy_*{net,renet,replic,lightyear}*` → No files found。**官方无 netcode crate**。
- 实读 `crates/bevy_time/src/virt.rs` 1-35：`Time<Virtual>` 可 pause / 倍速 / 单次更新最大推进截断；实读 `crates/bevy_ecs/src/change_detection/tick.rs` 13-32：`Tick`（`u32`）为变更检测时间基准，周期扫描保证相对年龄低于 `MAX_CHANGE_AGE`。
- **定性**：同步层的两个地基（逻辑帧时钟 + 变更 tick）由 ECS / Time 层提供，**复制本身交给外部 crate**。
- 生态（联网核查）：`bevy_replicon` MIT/Apache-2.0，**无内置 I/O、后端可插拔**（renet2 / quinnet / aeronet / matchbox）；`lightyear` Apache-2.0，30+ crate 按 `io` / `connection` / `replication`(prediction, interpolation) / `transport`(serialization) 分层，默认序列化 postcard，支持客户端预测回滚、快照插值、兴趣管理、lag compensation、带宽优先级。→ **传输层与同步层解耦是这两家的共同结构**。

### 1.3 UE（只读对照，一行不抄）：同步层是**独立子系统**

- 实读 `Runtime/Engine/Classes/Engine/NetDriver.h`（上一轮）：NetDriver 只管驱动 / 连接。
- 实读 `Runtime/Engine/Public/Net/RepLayout.h` 1-13：注释原文「core logic and types that support Object and RPC replication… focus on how data from Objects, Structs, Containers, and Properties are **generically tracked and serialized**」，主类 `FRepLayout`。
- 定位并实读 `Runtime/Net/Iris/Public/Iris/ReplicationSystem/ReplicationSystem.h` 1-40：独立目录 `Runtime/Net/Iris/`，`UReplicationSystem` / `UNetObjectFilter` / `UNetObjectPrioritizer` / `UDataStream` / `NetRefHandle` 等类型，与传统 `RepLayout` 并存。
- Iris 官方口径（联网核查，只读）：以**量化形式保存全量复制状态副本**；组件含 Replication States / Prioritization / Filtering / Net Serializers / Data Streams / Replication Bridge；支持 delta compression、量化、按连接的相关性过滤与优先级、复制与游戏线程数据分离以并行。
- **定性**：UE 把「传输（NetDriver）」「状态变更追踪与序列化（RepLayout / Iris）」「相关性过滤与优先级」分成不同子系统。KCP + msgpack 只对应最底一层的一小部分。

### 1.4 本项目历史约定（硬约束）

| 来源 | 关键条目 |
|---|---|
| `AGENTS.md`「自研边界与第三方库选型闸」（2026-11-09 用户拍板） | 关联系统**优先 vendored 成熟库**；硬闸 5 条：① 宽松许可 + LICENSE 原文随 vendored 入库 ② 与 MIT 兼容、**不得宣称绝对零风险**（MIT 无显式专利授权条款）③ 零构建期新依赖（header-only / 纯源码、不联网、不装 SDK、不引新运行时 DLL、MSVC14.38 / C++17 可编）④ `thirdparty/<lib>/VENDOR.md` 溯源 ⑤ `nl/` 薄封装隔离、业务代码不暴露第三方类型；UE 只读不抄、Carrot/bevy 抄思路不抄代码 |
| `docs/NON-RENDER-ROADMAP.md` L38 / L47 | 原定档「★★ 里程碑不直观 / 第三桶慎入」——**2026-11-13 Q-N1 拍板已作废**，两处已同步勘误为「已立项（第 12 步）」，此处保留为历史口径证据 |
| `docs/GAME-DEV-ORDER.md` §2（排期**唯一数据源**） | 原 11 步**完全不含多人 / 网络**——**2026-11-13 已改为 12 步**，新增第 12 步「多人游戏 / 网络栈」；第 7 步「最小可玩版」仍为单机开车闭环，第 12 步建议排其后 |
| `docs/MILESTONES.md` | 无网络里程碑；仅编辑器 RPC 的 msgpack 探针记录（L2753~2795 一带） |
| `docs/p8/P8-GAMELOOP-PLAN.md` §4 L38 | **「回滚 netcode」明确列为不在范围**（2026-11-09 拍板）→ 选回滚模型须重新拍板并同步勘误 |
| 同上 §2 / §3 / §6 | 已有 `nl::core::GameClock` 固定 60Hz 逻辑帧、`kMaxDelta=250ms`、暂停=时钟停摆、`alpha` 帧间插值输出（M-GL3 解禁）、命名 TickGroup（M-GL5 收官）、确定性回放契约（固定样本 + 固定 dt + PNG MD5 基线逐字节）→ **快照插值 / tick 编号的地基已具备** |

### 1.5 仓库现状与事实纠正（会改变结论）

1. **msgpack 已接入，不是从零引入**：`editor_bridge/msgpack_min.hpp` 自研极简编解码（header-only 零依赖，`std::variant` 只覆盖 nil/bool/int64/double/str/array/map，`std::map` 承载 map），跑本地 TCP / 命名管道，服务编辑器 RPC（`docs/editor/RPC-SPEC.md`）。它面向 RPC 信封 Value 树，**不适合逐帧状态同步**（需定长 POD / 位打包 / delta）。
2. **`RPC-SPEC.md` §2 L31 写「msgpack-c（header-only MIT，vendored 入库）」有误**：msgpack-c 实为 **Boost Software License 1.0**（非 MIT）；cpp-7.0.0 的 C++ 版**默认依赖 boost 头文件**，仅 `MSGPACK_NO_BOOST` / `MSGPACK_USE_BOOST=OFF` 实验性去 boost。按硬闸第 3 条默认形态**不合规**；且仓库实装是自研 min 版，文档与代码也不一致。→ 勘误挂账见 §7。
3. **KCP 不是网络库**：MIT、C89、`ikcp.h` + `ikcp.c` 两文件约 3000 行、零系统调用，纯 ARQ；**不负责 UDP 收发**，时钟外部传入、要自己写 output callback / `ikcp_update` 定时驱动 / socket / 连接管理 / 加密认证。符合硬闸，但「接入 KCP」≠「有联机」。
4. **上一轮结论修正**：上轮称「C++ 侧没查到等价的一体化复制库」，本轮定向检索查到 **RakNet / SLikeNet 系带 `ReplicaManager`（对象复制 + 序列化）**，确有覆盖 L3 的 C++ 库；但均为**一体化网络引擎**、体量大、需编译链接（见 §3），不是「可插拔 + ECS 原生 + 传输无关」的复制层。准确表述应为：**C++ 侧没有与 `bevy_replicon` / `lightyear` 同构的可薄插复制层；带复制层的库只有 RakNet / SLikeNet 这类整包网络引擎。**

---

## 2. 分层模型（本次范围 = L0~L4 全覆盖）

| 层 | 职责 | 用户原话覆盖 | 本次范围 |
|---|---|---|---|
| **L4 游戏逻辑** | GameClock 60Hz、输入 → 载具 → 相机 → World、交通 AI | — | ✅（消费端） |
| **L3 同步层** | 权威模型、快照 / 增量、插值、预测回滚、兴趣管理、延迟补偿 | ❌ | ✅ **「同步层也算上吧」追加** |
| **L2 序列化层** | msgpack / 位打包 / FlatBuffers、delta 编码 | ✅ msgpack | ✅ |
| **L1 传输层** | 可靠 / 不可靠 UDP、连接管理、分片、加密认证 | ✅ KCP | ✅ |
| **L0 UDP socket** | 系统收发；KCP 不含 I/O，需自己写 callback | — | ✅ |

---

## 3. 候选库核查（许可证 / 依赖 / 维护 / 硬闸判定）

| 库 | 层 | 许可证 | 形态与依赖 | 维护 | 硬闸判定 |
|---|---|---|---|---|---|
| **KCP** (skywind3000) | L1 | MIT | 纯 C `ikcp.h`/`ikcp.c` 两文件、零依赖、**零 I/O** | 活跃（镜像 2026-05 v2.1.1） | ✅ 完全合规；L1 以外全自研 |
| **ENet** (lsalzman) | L1 | MIT | 纯 C 单库，可靠 / 不可靠通道 + 连接管理 | 稳定 | ✅ 合规；无加密 / 认证 / 复制 |
| **GameNetworkingSockets** (Valve) | L1 | BSD-3-Clause | C++，可靠 / 不可靠 + 分片重组 + AES-GCM-256 + lanes + ICE NAT；**明确不做序列化 / 复制** | 活跃（2026-10 有提交） | ⚠️ 需 CMake + 加密依赖，撞硬闸第 3 条，须单独评审 |
| **yojimbo** (G. Fiedler) | L1 + 位打包 L2 | BSD-3-Clause | C++ client/server + connect token + 加密 + bitpacker；拖 netcode/reliable/serialize/libsodium | 2025-09 有提交 | ⚠️ 多子库 + premake/CMake，须单独评审 |
| **RakNet 4.081 / 现代化 fork** | L1~L3 | Simplified BSD / BSD-2-Clause | 一体化引擎，含 **ReplicaManager 对象复制** + 序列化 + NAT + RPC；CMake 3.14 / C++11 | 原版 discontinued；fork 现代化 | ⚠️ 体量大、需编译链接，撞硬闸第 3 条；复制模型绑定其对象系统，与本项目 ECS 不合 |
| **SLikeNet** | L1~L3 | MIT（派生 RakNet 部分为 BSD） | core 无第三方依赖；`_ext` 带 OpenSSL / libcatid | 社区续维（活跃度待核） | ⚠️ 同上；许可分层须逐文件入库核查 |
| **GGPO** | L3（回滚） | MIT | C/C++ **P2P 回滚**，要求引擎完全确定性 + save/load state + 无渲染帧推进；原版 Windows/VS2019/CMake，社区 fork 跨平台 | 2019 开源后核心稳定，fork 活跃 | ❌ 模型不匹配（开放城市 + 交通 AI + Jolt 非 bit 级确定性）且与 `P8-GAMELOOP-PLAN.md` §4「回滚 netcode 不在范围」冲突 |
| **GekkoNet** | L3（回滚） | 待核 | C/C++ P2P 回滚 SDK（GGPO / GGRS 灵感） | 较新 | ❌ 同上模型不匹配 |
| **msgpack-c** | L2 | **BSL-1.0** | C++ header-only 但**默认依赖 boost**；`MSGPACK_NO_BOOST` 实验性 | 活跃 | ⚠️ 许可非 MIT；默认形态撞硬闸第 3 条 |
| **自研 `msgpack_min.hpp`** | L2 | 本项目 | header-only 零依赖，Value 树 | 已落码 | ✅ 合规，但只够 RPC 信封 |
| **`bevy_replicon` / `lightyear`** | L3 | MIT/Apache-2.0 / Apache-2.0 | **Rust crate**，传输可插拔 | 活跃 | ❌ 语言不匹配；**只作分层思路参照**（抄思路不抄代码） |

> 两轮定向检索（「C++ server-authoritative replication」「C++ ECS replication / snapshot / delta」）未找到 MIT/BSD 且 header-only / 纯源码、可直接薄插进 `nl/` 的**可插拔状态复制层**；命中的通用 C++ 网络库（POCO / OpenDDS / RakNet 系）均非该形态。
> MIT / BSD / Apache 均无显式专利授权条款，按 `AGENTS.md` 硬闸第 2 条**不得对外宣称「绝对零风险」**；LICENSE 原文须随 vendored 入库。

---

## 4. 三层方案矩阵（待拍板，均不落码）

| 方案 | 组成 | 覆盖层 | 主要代价 | 与历史约定 |
|---|---|---|---|---|
| **A 原话直译** | KCP + msgpack（自研 min 或 msgpack-c 去 boost） | L1+L2 | **L3 全自研**；KCP 之外还要自己糊 socket / 连接管理 / 定时驱动 / 认证 | 与「关联系统优先成熟库」部分张力：L3 无成熟可插拔方案，属自研例外 |
| **B 传输一步到位** | ENet / GNS / yojimbo + 沿用自研编码 | L1+L2 | L3 仍全自研；GNS / yojimbo 需评审构建期依赖 | 同上 |
| **C1 同步层自研 + 传输可换** | 先定同步模型，参考 Iris（量化 / delta / 相关性 / 优先级）与 replicon/lightyear（传输可插拔）分层；传输层后选、经 `nl/` 薄封装隔离 | **L1~L3 全覆盖** | 最大工作量；须按「无成熟方案自研例外」**单独拍板**；建议第一步只做传输薄封装 + 可见 tool 卡，不碰游戏逻辑 | Q-N1 已拍板，★★ 定档已同步勘误（2026-11-13）；Q-N2~Q-N5 仍待拍板 |
| **C2 整包网络引擎** | SLikeNet / RakNet（含 ReplicaManager） | L1~L3 全覆盖 | 一体化引擎会把线协议 / 对象模型 / ECS 一起绑死；体量大、需编译链接 | 撞硬闸第 3 条与第 5 条（薄封装隔离），须单独评审 |
| **D 暂不立项** | 维持 11 步台账 + ★★ 定档，仅挂账 | — | 零成本 | 与现有约定零冲突 |

**代价速览**：新增依赖 A=1（KCP）／B=1（传输库）／C1=1~2／C2=整包；与既有拍板冲突 A/B=仅台账、C1=台账 + 自研例外、C2=台账 + 硬闸、D=无；可见验收形态 A/B=双进程 echo + 丢包延迟曲线 tool 卡，C1 追加「两客户端同场景位置一致」截图/数值锚，C2=库自带 sample，D=无。

---

## 4.1 序列化 / 消息协议候选全景（Q-N4，2026-11-13 补查）

> 触发对话：2026-11-13「那消息协议还有其他可选的么」。上表只覆盖了 msgpack 系与位打包两形态，本节补全候选并逐项过硬闸。检索结果标注日期 2026-11-12。
> **拍板结论（2026-11-13 用户拍板）**：**分层混用 = `msgpack_min.hpp` 保留（编辑器 RPC 专用）+ bitsery vendored（同步层位打包系）**。量化 / delta / 快照 diff 语义仍在其上自研；vendored 须按硬闸落实 `thirdparty/bitsery/VENDOR.md` + LICENSE 入库 + 本仓载荷实测（README 自报基准不作为验收证据）。**落码仍冻结至 Q-N2 / Q-N3 拍板。**

| 候选 | 许可证 | 形态与依赖 | 维护 | 硬闸判定 | 适合的层 |
|---|---|---|---|---|---|
| **自研位打包 + 量化** | — | 手写 | — | ✅ 零外部依赖 | **同步层快照 / delta**（UE Iris、Overwatch GDC 的工业口径均为此路；量化 + delta 语义必须自己写，位流原语可外包给库） |
| **bitsery** (fraillt) | MIT | header-only，C++11，**零依赖**，无 IDL / 无宏；位级控制扩展 + 前后向兼容扩展 | v5.2.5（2025-10 有提交） | ✅ **全部通过**（README 自报基准：体积 6913B 优于 msgpack/protobuf/flatbuffers，ser/des 时间最短；**作者自报数据，vendored 前须以本仓载荷实测**） | 同步层位流原语 + RPC 通吃 |
| 复用自研 `msgpack_min.hpp` | 本项目 | Value 树 | 已落码 | ✅ | 仅 RPC 信封，不适合逐帧快照 |
| msgpack-c | BSL-1.0 | header-only，**默认依赖 boost** | 活跃 | ⚠️ 见 §3 | RPC |
| FlatBuffers (Google) | Apache-2.0 | IDL + **codegen 步骤**；零拷贝读 | 活跃 | ⚠️ 构建期多一步 fbs 编译，撞硬闸第 3 条须单独评审 | 资源 / 配置 / 大快照随机访问 |
| Cap'n Proto | MIT | IDL + **codegen 步骤**；真零解析（结构即内存布局） | 活跃 | ⚠️ 同上 | 高频 IPC / RPC |
| Protobuf | BSD-3 | IDL + protoc；反序列化需构建对象树 | 活跃 | ⚠️ 同上，且实时同步层不占优 | 服务端 RPC |
| SBE | Apache-2.0 | IDL + codegen；金融低延迟出身 | 活跃 | ⚠️ 团队小众，生态弱 | 不推荐 |
| CBOR / BSON / cereal / yas | 各异 | 各异 | — | ❌ 相对 bitsery / msgpack 无增量优势 | 不推荐 |
| flecs meta 反射序列化 | MIT | 绑定 flecs ECS | 活跃 | ⚠️ 绑定特定 ECS | 仅当未来引入 flecs 才有意义 |

**关键对比事实**：
- bitsery 是查漏最大发现：MIT、零依赖、header-only、C++11、无 codegen，专为游戏实时网络设计，自带位级打包原语与跨平台对齐编译期检查——等于把「自研位打包」最容易出错的部分（位流读写、对齐、反序列化错误检查）验证过一遍，但**量化 / delta / 快照 diff 语义仍要自己写**。
- FlatBuffers / Cap'n Proto 的强项是**零拷贝读**（省反序列化开销），对资源加载与大快照随机访问有利；但两者都要 IDL codegen 步骤，且**并不能替代实时 delta 快照所需的位打包 / 量化**。

---

## 5. 建议（拍板前撰写，不代表后续落码口径）

- 若正式做多人，倾向 **C1**：同步层与传输层解耦、传输可替换，与 `nl/` 薄封装隔离铁律同向。第一步只做「可替换的传输层薄封装 + 能亲眼看到的 tool 卡（双进程 echo / 丢包下延迟曲线）」，不碰游戏逻辑；同步模型另开一次拍板。
- 同步模型倾向 **服务器权威 + 快照插值**：与既有 GameClock 60Hz / `alpha` 插值 / TickGroup / 确定性回放契约同向，且与 `P8-GAMELOOP-PLAN.md` §4 排除回滚一致。⚠️ 这是倾向，**不是结论**。
- **传输层建议（2026-11-13 补）→ 同日用户拍板采纳 = ENet**：① 连接管理属关联系统，按 2026-11-09 拍板原则用成熟库（KCP 是纯 ARQ，连接管理 / socket / 双通道拆分全自研，不占自研例外三条任何一条）；② 第 12 步第一目标是最小可玩联机闭环，ENet 可靠/不可靠双通道直接对上「可靠=控制消息 / 不可靠=快照流」需求形态；③ 传输层本就可插拔，经 `nl/` 薄封装隔离后 KCP 是底下的候选替换项，延迟优化触发条件出现再换。已知边界如实登记：ENet 无加密 / 认证（不阻塞第一步）；相比 KCP 纯算法形态封装黑一点 → 薄封装隔离为铁律。
- 不建议 GGPO / 回滚：与本场景（开放城市 + 交通 AI + Jolt 非 bit 级确定性）及既有拍板双重不匹配。
- 不建议直接上 SLikeNet / RakNet：一体化引擎会把 ECS / GameClock / 线协议一起绑死，违背薄封装隔离。

**序列化（Q-N4）建议（2026-11-13 补查后追加）→ 同日用户拍板采纳**：
- **分层混用**是工业常态：RPC / 编辑器信封**保留 `msgpack_min.hpp`**（已落码、零成本、够用）；**同步层另走位打包系**——快照 / delta 需要定长量化编码，Value 树形态不适合逐帧状态。
- 同步层位打包二选一：**bitsery vendored**（MIT、零依赖、header-only、位流原语已验证，符合「关联系统优先成熟库」拍板原则）**或纯手写**（零新增依赖，但位流对齐 / 跨平台 / 错误检查的坑要自己踩；UE Iris 即手写路线）。倾向 **bitsery**：按 2026-11-09 拍板「用成熟库是通例，自研是例外」，序列化管道属关联系统，非 nanite / lumen 主线；且其量化 / delta 语义仍由我们在其上自研，核心不受第三方绑定。vendored 前须按硬闸落实 `thirdparty/bitsery/VENDOR.md` + LICENSE 入库 + 本仓载荷实测。

---

## 6. 待拍板清单（开工闸：架构判定未清不许落码）

| 编号 | 问题 | 选项 |
|---|---|---|
| **Q-N1** | 多人游戏是否正式进路线？是否改 11 步台账与 ★★ 定档？ | ✅ **已拍板（2026-11-13）= 正式进路线**；已写回第 12 步 + 同步勘误旧定档 |
| **Q-N2** | **同步模型**（决定后续所有形态） | ✅ **已拍板（2026-11-13）= 服务器权威 + 快照插值**：服务器 60Hz 算载具物理 → 快照流（ENet 不可靠通道）→ 客户端 alpha 插值显示；与 GameClock / `alpha` / TickGroup 地基同向，`P8-GAMELOOP-PLAN.md` §4 回滚排除条款无需解禁 |
| **Q-N3** | 传输层 | ✅ **已拍板（2026-11-13）= ENet vendored**（MIT、纯 C、可靠/不可靠双通道 + 连接管理；经 `nl/` 薄封装隔离保持可插拔，KCP 保留为封装底下的候选替换项）。入库走硬闸（`thirdparty/enet/VENDOR.md` + LICENSE 入库），落码冻结至 Q-N2 拍板 |
| **Q-N4** | 序列化口径与现有资产 | ✅ **已拍板（2026-11-13）= `msgpack_min.hpp` 保留（编辑器 RPC）+ bitsery vendored（同步层位打包）**；量化 / delta 语义自研其上；vendored 走硬闸（VENDOR.md + LICENSE + 实测），落码冻结至 Q-N2 / Q-N3 |
| **Q-N5** | 范围边界 | ✅ **已拍板（2026-11-13）= 最小闭环**：第一版只同步载具 transform（位置/朝向/速度）+ 玩家加入退出；专用服务器形态（headless / listen 可选）；**不纳管 editor RPC**（两通道独立，`RPC-SPEC.md` 轨道不动） |
| **Q-N6** | 勘误 | ✅ **已拍板（2026-11-13）= 现在改**：`RPC-SPEC.md` 三处（§2 L31 / §11 L249 / §12 L292）已同步勘误为「自研 `msgpack_min.hpp` 实装、msgpack-c 为 BSL-1.0 未 vendored」，全仓 grep 复核无残留裸旧口径 |

---

## 7. 勘误挂账

- ✅ **已结（2026-11-13，Q-N6 拍板后执行）**：`docs/editor/RPC-SPEC.md` §2 L31 / §11 L249 / §12 L292 三处「msgpack-c（header-only MIT，vendored 入库）」均已勘误为「实装 = 自研 `editor_bridge/msgpack_min.hpp`（零依赖）；msgpack-c 为 BSL-1.0 非 MIT 且 C++ 版默认依赖 boost，未 vendored」。全仓 grep（`msgpack-c.*MIT` 等）复核：`docs/` 下无残留裸旧口径，仅本文件留痕引用。
- 上一轮对话结论「C++ 侧没查到一体化复制库」→ 以 §1.5(4) 为准（RakNet / SLikeNet 带 ReplicaManager，但形态不匹配）。

---

## 8. 留痕

- 2026-11-13（方向探讨阶段）：用户追加「同步层也算上吧」→ 范围扩为 L0~L4 全覆盖，本文件为该阶段唯一产物；无代码 / 台账 / 配置改动，无 track / claim / tag / 回归。
- 2026-11-13（续）：用户拍板「之前没打算做 现在想做了 加到计划里」→ **Q-N1 = 正式进路线**。写回 `docs/GAME-DEV-ORDER.md`（11→12 步、新增第 12 步、§3 结构性判断、§4 待拍板表、§5 变更记录），同步勘误 `docs/NON-RENDER-ROADMAP.md`（头部追加拍板 + L38 / L47 / L67），并更新本文件状态头 / §0 / §1.4 / §4 / §6。**Q-N2~Q-N6 未拍板 → 开工闸冻结落码**；本轮零代码改动、未建 track / 未 claim / 未打 tag / 未跑回归。
- 2026-11-13（续 2）：用户问「消息协议还有其他可选的么」→ 补查并新增 **§4.1 序列化候选全景**（bitsery / FlatBuffers / Cap'n Proto / Protobuf / SBE / CBOR 等 10 项过硬闸），§6 Q-N4 行与 §5 建议同步追加「分层混用 + bitsery 倾向」推荐。
- 2026-11-13（续 3）：用户问「还要不要 msgpack_min」→ 结论 = 保留（两个消费端不竞争：编辑器 RPC 低频变长 Value 树 vs 同步层 60Hz 定长量化）。**用户拍板「msgpack_min + bitsery 吧」→ Q-N4 = 分层混用**。写回本文件状态头 / §4.1 拍板结论 / §5 / §6 / §8，`GAME-DEV-ORDER.md` 头部与 §4 待拍板行同步。bitsery 入库动作挂到 Q-N2 / Q-N3 拍板后的落码期。**Q-N2 / Q-N3 / Q-N5 / Q-N6 仍未拍板 → 开工闸冻结落码**；本轮零代码改动、未建 track / 未 claim / 未打 tag / 未跑回归。
- 2026-11-13（续 4）：用户问 KCP 有无问题 → 结论 = 库本身过硬闸无争议（MIT / C89 两文件 / 零依赖 / 活跃），边界在「纯 ARQ 非传输库」：不含 socket / 连接管理 / 认证，且只有可靠有序一条通道，快照流须另配裸 UDP 丢旧保新。用户问「推荐 ENet?」→ 给出推荐（连接管理属关联系统按 2026-11-09 原则用成熟库 / 双通道直接对上需求 / 传输可插拔 KCP 留作替换项）。**用户拍板「那就用 ENET」→ Q-N3 = ENet vendored**。写回本文件状态头 / §5 / §6 / §8，`GAME-DEV-ORDER.md` 头部与 §4 行同步。ENet 入库走硬闸，落码冻结至 Q-N2 拍板。
- 2026-11-13（续 5，Q-N2 / Q-N5 / Q-N6 拍板 + 开 PLAN）：用户指令「开 NETCODE-PLAN.md 走落码流程」→ 按开工闸先把剩余三项摆选项一次确认：**Q-N2 = 服务器权威 + 快照插值；Q-N5 = 最小闭环（只同步载具 transform + 加入退出；专用服务器形态；不纳管 editor RPC）；Q-N6 = 现在改勘误**。勘误已执行（`RPC-SPEC.md` 三处 + 全仓 grep 复核，§7 结案）；拍板写回状态头 / §6 / §8。**Q-N1~Q-N6 全部拍板完毕，开工闸解除**；产出 `docs/net/NETCODE-PLAN.md`（M-NET0~M-NET3 落码路线）。本轮仍零代码改动（PLAN 属文档）；未建 track / 未 claim / 未打 tag / 未跑回归——落码自 M-NET0 起，按 §0 归属声明 + claim 后开工。
- 联网检索结果标注日期 2026-11-12（搜索引擎返回）；许可证与维护状态以各上游 LICENSE / README 为准，**vendored 前须按硬闸逐项复核**。
