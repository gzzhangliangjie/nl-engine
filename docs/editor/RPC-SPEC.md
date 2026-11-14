# nl-engine 编辑器 ↔ 引擎 RPC 协议规范（RPC-SPEC）

> 状态：草案 v2（ImGui 路线，待评审拍板）
> 归属：编辑器外置轨道（独立于渲染主线 S*/M* 里程碑）
> 参照：`D:\SunshineOfficial`（编辑器侧架构范式：PluginManager/扩展点/WidgetLib/GameWidget 嵌入控件思想）与 `D:\SunshineClient`（引擎侧 SDK，`SunshineSDK/SunshineClient.py` + `simplerpc`）。**编辑器宿主已定：C++ + Dear ImGui（非 Python/PyQt5，非 Qt）**，仅借鉴其架构思想，不复用其代码。

---

## 1. 目标与范围

为「外置编辑器」（独立进程）与「nl-engine 渲染程序」（独立进程）之间定义一套**双向 RPC**，满足：

1. 两进程独立，一方崩溃不影响另一方；
2. 通过 RPC 交互（控制指令 / 事件上报）；
3. 引擎原生窗口经 Win32 `SetParent` 嵌入编辑器窗口（**像素直出屏幕，绝不走 RPC**）；
4. 编辑器侧以插件形式扩展多种工具，插件经 RPC 驱动引擎。

**本规范只定义 RPC 通道**（协议、方法、生命周期、错误码、两侧接口契约）。窗口嵌入、插件体系、UI 不在本文范围。

---

## 2. 与 Sunshine 参照的关系（复用 vs 偏离）

| 维度 | Sunshine 做法 | 本规范决策 |
|---|---|---|
| 编辑器宿主 | Python 3.11 + PyQt5（qtpy） | **改为 C++ + Dear ImGui**：借鉴 Sunshine 的 PluginManager/扩展点/WidgetLib/GameWidget 思想，用 C++ 自研轻量插件体系；ImGui 负责宿主 UI 与视口嵌入控件 |
| 引擎侧语言 | Python（SunshineClient 跑在引擎 Python 环境） | **改为 C++**（nl-engine 本身是 C++17/NVRHI） |
| 线协议 | JSON-RPC 2.0，经 `simplerpc` 跑 TCP（`simplerpc` 为 Python-only、vendored 在 SDK 内） | **保留 JSON-RPC 2.0 语义结构**，但传输层换为 **msgpack + 4 字节长度前缀**；不引入 simplerpc |
| 行为语义 | 插件方法注册 + Proxy + `OnPluginReady/OnPluginClose` + `Update()` 泵 + 心跳 | **完整复刻**到 C++ 侧 |

> 偏离理由：① 编辑器宿主从 Python/PyQt5 改为 **C++ + Dear ImGui**——纯 C++（MIT，免费，零运行时依赖）与引擎共享类型，契合本项目 vendored 习惯；Sunshine 的插件体系/嵌入控件仅作架构思想借鉴，不复用其 Python 代码。② 线协议同理：`simplerpc` 是 Python 库无法在 C++ 侧复用；两侧统一用 **自研 `editor_bridge/msgpack_min.hpp`（header-only 零依赖，Value 树）**，序列化与引擎侧同源。（2026-11-13 勘误：本行原写「`msgpack-c`（header-only MIT，vendored 入库）」有误——msgpack-c 实为 **BSL-1.0** 非 MIT，且其 C++ 版默认依赖 boost 头文件，仓库从未 vendored 该库，实装始终为自研 min 版。）JSON-RPC 2.0 的 `method/params/id/result/error` 结构对调试友好，保留语义、替换编码。

---

## 3. 传输层

- **默认传输**：本地 TCP，`127.0.0.1`，默认端口 `29110`（避 Sunshine 的 29102）。编辑器 `--rpc-endpoint <ep>`、引擎 `--editor-endpoint <ep>` 覆盖；两端必须给同一个 endpoint（早期文档写的 `--rpc-port` 从未实装，M-E6 一并勘误）。
- **endpoint 语法（M-E6 实装）**：`tcp://127.0.0.1:29110` / `tcp://127.0.0.1`（省略端口取 29110）/ `pipe://<name>` / 裸端口号。`parseEndpoint` 只允许 loopback 主机 `127.0.0.1` / `localhost`，其它 host 与未知 scheme 直接报 `bad endpoint` 并禁用连接，**不静默回落到默认端口**。
- **命名管道（M-E6 实装，可选）**：`pipe://<name>` 映射到 `\\.\pipe\<name>`；字节流模式 `PIPE_TYPE_BYTE | PIPE_READMODE_BYTE` + `FILE_FLAG_OVERLAPPED` 异步读写，`PeekNamedPipe` 做可读探测，`PIPE_UNLIMITED_INSTANCES` 支持多引擎并发，每 accept 成功立即 arm 下一个实例。两类传输**共用同一套上层协议**（`editor_bridge/transport.hpp` 的 `Stream` / `Listener` 抽象），仅 endpoint 字符串不同。
- **连接模型**：**编辑器为 Server（监听），引擎为 Client（主动连接）**——对齐 Sunshine（引擎侧 `SDK_CLIENT` 连回编辑器 `Sunshine` server）。M-E6 起 Server 可同时持有多个 client，按 `viewId` 路由（见 §9 / §10）。
- **字节序**：msgpack 多字节整型按 msgpack 规范走**网络字节序（大端）**；4 字节长度前缀亦为**大端 uint32**（见 §4 勘误）。早期文档写作 LE 是笔误，实装自 M-E1 起从未变过。

---

## 4. 分帧（Framing）

每条消息 = `4 字节 BE uint32 长度 N` + `N 字节 msgpack 编码的载荷`。

> **M-E6 勘误**：本节与 §3 早期写「LE / 小端」为文档笔误。C++ 实装（`rpc_host.cpp` / `rpc_bridge.cpp`）与历代 python 探针自 M-E1 起一律用大端（`>I`）。M-E6 **只勘误文档、不改线格式**，以保证既有版本卡 / 探针零变更。

- 单个载荷上限 **8 MiB**（与 Sunshine `MAX_MESSAGE_LENGTH` 对齐）；超长直接断开并记错误。
- 接收方按长度精确切分，无需分隔符；支持一个 TCP 包内粘多个消息、或一个消息跨多个包。
- 理由：长度前缀比 JSON-RPC 常见的换行分隔更鲁棒，且对二进制 msgpack 天然友好。

---

## 5. 消息信封（Envelope）

载荷为 msgpack map，结构对齐 **JSON-RPC 2.0**：

```
{
  "jsonrpc": 1,            # int 标记，固定 1（代表本规范 v1）
  "id":      <uint>,       # 请求/响应关联 id；0 = 单向通知（无响应）
  "type":    "req" | "resp" | "notify",
  "method":  <str>,        # req/notify 必有
  "params":  <array>,      # 位置参数（首选）；亦可用 "kw" 关键字 map
  "ok":      <bool>,       # resp 必有
  "result":  <any>,        # resp 且 ok=true 时存在
  "error":   { "code": <int>, "message": <str> }  # resp 且 ok=false 时存在
}
```

- 请求方生成自增 `id>0`；响应方原样回填该 `id`。
- `type="notify"`（`id=0`）：只下发、不期待响应（引擎→编辑器事件、心跳）。
- `params` 统一用**位置参数数组**，便于两侧映射；复杂结构用嵌套 map/array。

---

## 6. 角色、方向与命名空间

仅两个对等端：**编辑器（Editor / host）** 与 **引擎（Engine / peer）**。方法名以命名空间区分归属：

| 前缀 | 调用方向 | 含义 |
|---|---|---|
| `engine.*` | 编辑器 → 引擎 | 编辑器命令引擎（如 `engine.setCamera`） |
| `editor.*` | 引擎 → 编辑器 | 引擎调用编辑器服务（如 `editor.log`） |
| `rpc.*` | 双向 | 基础设施（握手 / 心跳 / 版本） |

---

## 7. 调用语义

- **请求-响应**：调用方 `call(method, params, timeoutMs)`，阻塞或回调等待匹配 `id` 的 `resp`；超时按错误码 `408` 处理。
- **单向通知**：`notify(method, params)`，立即返回，无响应。
- **并发**：单连接、单客户端；入站消息在 `update()` 泵中串行分发（引擎侧在主循环每帧调用 `bridge.update()`）。
- **方法未找到 / 参数错误**：响应 `ok=false` + 对应错误码，不中断连接。

---

## 8. 错误码

| code | 含义 | 处理 |
|---|---|---|
| 0 | 成功 | — |
| 100 | 方法不存在 | 调用方检查拼写 |
| 101 | 参数非法（类型/数量错） | 调用方修正 |
| 102 | 处理器内部异常 | 被调方日志，返回 message |
| 103 | 引擎未就绪（announce 前收到业务调用） | 调用方等待 `OnReady` |
| 200 | 协议版本不匹配 | 断开，提示升级 |
| 408 | 调用超时 | 调用方超时回调 |
| 500 | 未分类处理器异常 | 记日志 |

---

## 9. 连接生命周期（握手 / 心跳 / 重连）

```
[引擎启动]                [编辑器启动]
    |                          |
    |  listen (TCP server)    |
    |<-------------------------| connect (TCP client)
    |                          |
    |  engine.announce{pid,hwnd,proto,caps}  ──notify──►  (编辑器收到后 SetParent 嵌入视口)
    |                          |
    |  OnReady 触发            |  OnPluginReady 触发
    |                          |
    | 每 2s: rpc.ping ────────►│
    |◄──────────── rpc.pong    |  (任一方 6s=3 次未收到 → 判对方掉线)
    |                          |
[引擎崩溃/断开]            [编辑器掉线]
    |  onClose/onClose         |
    |  编辑器显示空视口+重连UI  |  引擎继续自渲染（崩溃隔离！），等待重连
    |<--- 每 1s 重连尝试 ------|
```

- **心跳**：引擎（client）每 2s 发 `rpc.ping`（notify），编辑器回 `rpc.pong`（notify）；任一方超过 `heartbeatTimeout`（默认 6s）未收到对端任何消息，判定断开。
- **崩溃隔离**：编辑器掉线时**引擎不得退出**，继续渲染自有窗口；编辑器掉线重连后重新 `announce`。
- **编辑器掉线恢复**：编辑器显示「引擎未连接」空窗 + 自动重连（默认 1s 间隔），重连成功重走握手。

**M-E6 多连接 / 多视口补充**
- Server 持有 `vector<unique_ptr<Client>>`，每帧 `update()` 最多 accept 16 个连接，逐连接非阻塞 `waitReadable(0)` + `recvSome` + 粘包解析，不再假设「全局唯一 socket」。
- `engine.announce` 增字段 `viewId`（缺省 `main`，老插件零改动兼容）；编辑器按 `viewId` 建独立 ImGui 视口 + 独立 `WS_CHILD` 容器，各挂一个引擎 HWND。重复 `viewId` 记 `[rpc] warning: duplicate viewId '...'`，同 key 覆盖而不新增容器。
- `rpc.pong` **按连接定向回复**（单 client 时代抓全局 socket 的写法已废）；`RpcHost::callTo/notifyTo(viewId, ...)` 精确路由，`callTo` 未知视图返回 `RpcError{103,"unknown view"}`；`call()`/`notify()` 打给 `activeView`（首个连入的视图自动设为活动视图，UI 可切 Combo），找不到时回落首个连接。
- 单个 `viewId` 掉线只拆该视图容器，其余视图与编辑器主循环不受影响；`onEngineClose(viewId)` 是新签名。
- **明确不做**：单引擎进程多 HWND / 多 swapchain（本轨道范围内否决）；「多视口」语义恒为「一个编辑器接纳多个引擎进程」。

---

## 10. 初始方法注册表（分期落地）

### M-E1 握手 + 心跳 + echo
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `rpc.version` | 双向 | — | `{proto:1}` |
| `rpc.ping` | 引擎→编辑器 | —（notify） | — |
| `rpc.pong` | 编辑器→引擎 | —（notify） | — |
| `engine.announce` | 引擎→编辑器 | `{pid:int, hwnd:uint64, proto:int, caps:map}`（notify） | — |
| `engine.echo` | 编辑器→引擎 | `[any]` | 原样回显 |

### M-E2 视口嵌入
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| （编辑器侧动作） | — | 收到 `engine.announce` 后 `SetParent(hwnd, frameHWND)` + 裁剪 | — |
| `engine.setViewportSize` | 编辑器→引擎 | `{w:int,h:int}` | `ok` |
| `engine.viewportResized` | 引擎→编辑器 | `{w:int,h:int}`（notify，用户拖动引擎窗时） | — |

### M-E3 第一个工具插件（以相机/场景树为例）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `engine.setCamera` | 编辑器→引擎 | `{pos:[f,f,f], target:[f,f,f], up:[f,f,f]}` | `ok` |
| `engine.getSceneTree` | 编辑器→引擎 | — | 嵌套 map 场景图 |
| `engine.focusEntity` | 编辑器→引擎 | `{path:str}` | `ok` |
| `editor.sceneChanged` | 引擎→编辑器 | `{}`（notify，引擎侧场景变更） | — |
| `editor.log` | 引擎→编辑器 | `{level:str, msg:str}`（notify） | — |

> 方法表随插件增长扩展；新增方法须在本文「注册表」追加并标注所属里程碑。
> 注：M-E3/M-E4 实装时方法名细化为 `camera.*` / `scene.*` 命名空间（与上表初稿的差异
> 属命名收敛，语义一致）；以 M-E4/M-E5 实装表为准。

### M-E4 实装（相机 + 场景树）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `camera.getState` | 编辑器→引擎 | — | `{fov,dist,yaw,pitch}`（度） |
| `camera.setFov` | 编辑器→引擎 | `{fov:deg}` | clamp 后实际 fov |
| `camera.setDistance` | 编辑器→引擎 | `{dist}` | clamp 后实际 dist |
| `camera.orbit` | 编辑器→引擎 | `{dx,dy}`（像素增量） | `{yaw,pitch}` |
| `camera.reset` | 编辑器→引擎 | — | nil ack |
| `scene.getState` | 编辑器→引擎 | — | `{mode,count,objects:[{mesh,pos,tint,rough,metal}],yaw,pitch,dist}`（model 模式 objects 为空表） |
| `scene.focus` | 编辑器→引擎 | `{i:int}` | `{yaw,pitch,dist}`；坏索引 error 100 |

### M-E5 实装（材质检查）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `scene.getMaterials` | 编辑器→引擎 | — | scene 模式 `{mode:"scene",count,objects:[{i,mesh,tint:[3],rough,metal}]}`；model 模式 `{mode:"model",file,nmesh:{name,meshlets,submeshCount,submeshes:[{materialId,clusterFirst,clusterCount}]}}`（.nmesh NSUB 表，材质 id=子网格序号；非 .nmesh 时 nmesh=null） |
| `scene.setMaterial` | 编辑器→引擎 | `{i:int, tint?:[3], rough?:f, metal?:f}`（逐字段可选，clamp 0..1） | `{ok:1,i,tint,rough,metal}`；model 模式 / 坏索引 error 100 |

### M-E6 实装（多视口路由 + 编辑器侧管理）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `engine.announce` | 引擎→编辑器 notify | `{pid,hwnd,proto,caps,viewId}`（`viewId` 缺省 `main`） | —（编辑器收到后建容器 + `SetParent`） |
| `editor.views` | 任意→编辑器 | — | `[{viewId,hwnd,pid,proto,attached}]`（当前全部连入视图） |
| `editor.pingView` | 任意→编辑器 | `{view:<id>}` | `{sent:bool,view}`；向该视图定向 `notifyTo` 一条 `editor.probe` |
| `editor.callView` | 任意→编辑器 | `{view:<id>,method:<str>,params?}` | 转发 `callTo(view, method, params, 3000)` 的 `RpcResult`；缺 view/method → error 100，未知 view → error 103 |

> `editor.views` / `editor.pingView` / `editor.callView` 由 `editor/main.cpp` 注册在 host 上，供探针与插件查询/定向调用；引擎侧 handler 集合（`engine.echo` / `camera.*` / `scene.*`）M-E6 零改动。

### M-E7 实装（模型预览 + cvar 反射通道）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `camera.preset` | 编辑器→引擎 | `{name:str}`：`front`(yaw0,pitch0) / `back`(180) / `left`(90) / `right`(-90) / `top`(pitch→83.0789 clamp) / `iso`(45, 35.264) | `{name,yaw,pitch,dist}`（回读实际值；`top` pitch 钳到 OrbitCamera 上限 ±1.45 rad）；未知 preset error 101 |
| `engine.cvar.list` | 编辑器→引擎 | `{prefix?:str}` | `[{name,type,value,help}]`（Console 全量 cvar 按 prefix 过滤） |
| `engine.cvar.get` | 编辑器→引擎 | `{name:str}` | `{name,type,value,help}`；未知 cvar error 100 |
| `engine.cvar.set` | 编辑器→引擎 | `{name:str, value:str}`（`setFromString` 语义） | 更新后的 `{name,type,value,help}`；未知 cvar error 100、类型拒绝 error 101 |

> **参数通道口径（拍板）**：`engine.cvar.*` 是反射胶水层的 RPC 面——引擎原生参数注册成 cvar（M-E7 落了 `light.sunAzimuth/sunElevation/ambient` 三个），脚本组件字段后续走 M-SC10 `[Prop]` 描述表；RPC 只做传输，不再手写每参数 handler（M-E5 的 `scene.setMaterial` 保留为既有面）。
> **CLI 旗标（viewer）**：`--preview` = mode scene + `assets/scenes/preview.scene.json`（用户显式 `--mode`/`--scene` 优先）；`--preview-model <glb名>` = 预览场景 JSON 反序列化后按实体名 `preview-model` 覆写 meshFile（坏名 exit 2）。

### M-E8 实装（场景树层级 + 变更推送）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `scene.getTree` | 编辑器→引擎 | — | `{mode, count, nodes:[{id:u64, name:str, parent:u64(0=无父), idx:i64(-1=无 Model 实例行), kind:"model"\|"light"\|"empty", mesh?, tint?:[3], rough?, metal?, lightType?, intensity?}]}`（World 全集实体含无 Model 的灯/空实体；idx = sceneObjects 下标，仅 kind="model" 可用 focus/setMaterial；roots 由编辑器侧按 parent==0 归组，悬空父按根处理） |
| `editor.sceneChanged` | 引擎→编辑器 notify | `{}` | —（引擎每帧对 World 全集算 FNV1a 指纹（id/name/parent + Model tint/rough/metal/mesh + Light type/intensity/color/direction），变化帧 notify 一次；**tick off 的 preview/scene 也生效**（指纹在 ticking 块外）。编辑器侧 `registerNotify` 置 `g_sceneTreeDirty`，SceneOutlinerPlugin 200ms 防抖后自动重拉 getTree，替换手动 Refresh） |

> **M-E8 行为变更（覆盖 M-E5 口径）**：`scene.setMaterial` 的 tint/rough/metal 改动**写回 World `ModelComponent`**（源头单写）——M-E5 原实现只改渲染收集表 `sceneObjects`，tick 开启时下一帧即被 World 旧值冲掉、树/指纹无感；M-E8 起写回 World，帧循环自然传导、指纹捕获、scene.getTree 同步。M-E7 探针既有 setMaterial 断言复验 0 fail。

### M-E9 实装（场景树编辑操作）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `scene.rename` | 编辑器→引擎 | `{id:u64, name:str}` | `{ok:1, id, name}`；未知实体 error 100 / 参数缺失 error 101 |
| `scene.setParent` | 编辑器→引擎 | `{id:u64, parent:u64(0=解除)}`（纯关系，不动变换） | `{ok:1, id, parent}`；未知实体/父 error 100，parent==self / 成环 error 101（World assert 的 RPC 化） |
| `scene.reparent` | 编辑器→引擎 | `{id:u64, parent:u64(0=解除)}`（保世界变换挂父） | 同 setParent |
| `scene.addEntity` | 编辑器→引擎 | `{name?:str, parent?:u64, kind:"empty"\|"light"\|"model", mesh?:str, lightType?:0\|1, intensity?:f}` | `{ok:1, id, name, kind}`；model 自动挂 Transform（收集段硬要求）；meshFile 白名单 + **存在性校验**（`NL_ASSET_DIR` 下 is_regular_file，防坏实体触发 rebuild → loadEcsScene 失败 → 渲染循环退出）；校验失败 error 101；未知 kind / 点光 shadow error 101；未知父 error 100；flush 即时落表 |
| `scene.removeEntity` | 编辑器→引擎 | `{id:u64}`（父删递归删子；flush 即时落表） | `{ok:1, id}`；未知实体 error 100 |
| `editor.sceneChanged` | 引擎→编辑器 notify | `{}` | 指纹扩域（M-E9/M-E10）：+ Light position/range/shadow + Transform position/scale/rotation |

> **M-E9 渲染传导（M-SC7 块移出 ticking 扩灯集）**：集合签名 = Model(id, meshFile) + 灯实体 id 序（从 World 现算——`lightIds` 只在 rebuild 更新不能当签名源）；**签名比对移至帧尾 ticking 块外**（tick on 语义等价：集合稳定点 = 帧尾；tick off 的 preview/scene 缺省同样每帧比对）；rebuild = 重收 modelIds/objects/lights/lightIds + preview key 重注入 + loadEcsScene（initScene 可重入）+ **重建后实例数据当帧上屏**（本块在 ticking 上传点之后执行）。灯参数变更不走重建（setProp 直写 lights[i] + updateEcsLights 单 upload）。
> **编辑 handler 仅 scene 模式注册**（bindSceneEdit；model 模式 = method not found error 100）。

### M-E10 实装（对象级参数泛化面）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `scene.getProps` | 编辑器→引擎 | `{id:u64}` | `{id, name, props:[{component:"Transform"\|"Model"\|"Light", name:str, type:"float"\|"vec3"\|"color"\|"quat"\|"int"\|"bool"\|"string", value, min?, max?}]}`（显式描述表遍历；M-SC10 PropAttribute 同构口径：name/type/Range 显式注册，Default 不注册 = 现值快照）；未知实体 error 100 |
| `scene.setProp` | 编辑器→引擎 | `{id:u64, component:str, name:str, value}`（显式分发，反射胶水层终点——RPC 只传输） | `{ok:1, id, component, name}`；写回 World 源头单写 + 渲染直写：Transform → objects[i].world 直刷；Model 材质 → objects[i] 三字段直刷；Light → lights[i] 重算 + updateEcsLights 单 upload；meshFile → 集合签名重建。未知 prop/组件 error 100，类型不符 / 点光 range≤0 / 点光 shadow / meshFile 白名单或不存在 error 101（点光约束写前拒绝不留半态） |

> **M-E10 描述表现量**（新增组件字段 = 引擎侧表加一行，面板零改动）：Transform.position/scale(vec3)/rotation(quat)、Model.meshFile(string)/tint(color)/roughness/metallic(float 0..1)、Light.type(int 0..1)/direction(vec3)/color(color)/intensity(float 0..20)/position(vec3)/range(float 0.1..100)/shadow(bool)。编辑器侧 `PropertyPanelPlugin`（Properties 面板）按 type 渲染通用控件；`SceneOutlinerPlugin` 首次拉取自动选中首个根节点（`g_sceneSelEntity` 共享选中态，离断桌面会话合成点击不可靠 → 面板证据走确定性路径）。

### M-E12 实装（编辑 undo-redo）
| 方法 | 方向 | 参数 | 返回 |
|---|---|---|---|
| `scene.undo` | 编辑器→引擎 | `{}` | `{ok:1, undoDepth, redoDepth}`；栈空 error 102 / 重建失败 error 103 |
| `scene.redo` | 编辑器→引擎 | `{}` | 同 undo |

> **M-E12 命令栈语义**（viewer 侧单源，RPC 与 GUI 编辑共用；深度上限 128）：五类编辑操作成功路径记录逆操作（`editPushUndo` 清 redo 栈——新编辑作废重做分支）：Rename{sa/sb 旧新名}、Link{parent/parentNew 旧新父 + wasReparent 通道记忆}、Prop{grabProp 写前旧值 + 写后回读新值（含钳制真值），comp/name/kind 载荷}、Structural{删除**前**子树快照（拓扑+组件全字段）+ **undoIsDelete 方向位**}。Rename/Link/Prop 双向执行体同构 = 执行当前正向侧 + swap 双值 + 压对侧栈；Structural 双向 = doStructural(act)：act=true 删除（重抓快照保 removed 与 id 对齐）、act=false 重建（活父直挂/快照父映射）→ **remapStacks 修正双栈顶层与快照内部全部 id 引用**；动作由来源栈决定（undo 栈 act=undoIsDelete、redo 栈取反，执行后不翻转——同 cmd 双栈往返动作交替由栈来源天然保证）。空栈 error 102；编辑器侧 Undo/Redo 按钮 + Ctrl+Z/Ctrl+Y。
> **闭包存活口径（本轮坑）**：bindSceneEdit 局部 lambda（snapshotSubtree/pushStructural/applyPropValue/applyPropSide/execUndo/execRedo）被 handler 捕获后随 bridge handler 表持久存活——**按引用捕获局部量 = 悬垂 UB**，全链按值捕获（M-E9 既有 handler 同口径）。

### M-E13 实装（outliner 多选 + 框选）
| 面 | 机制 | 说明 |
|---|---|---|
| Ctrl+点击 | `selSet_` toggle | 不驱动 focus（避免 orbit 相机乱跳）；`g_sceneSelEntity` 恒 = 最后点击（Properties 面板保持单选消费） |
| Shift+点击 | 范围选 | nodes_ 序 between 上次点击与本次（锚 = lastClickId_） |
| 框选拖拽 | `drawBoxSelect` | 树区左键拖 >4px 画前景 overlay 矩形；松开收集可见节点 item rect 相交（UE 同口径相交即选）；`box-selected N` 状态行 |
| 批量操作 | Delete selected / Reparent selected | 循环走既有 scene.removeEntity / scene.reparent（viewer undo 栈逐条 push，Undo 可撤） |

> **M-E13 验收口径（确定性路径拍板延续）**：SendInput 点击链被用户桌面 TOPMOST 遮挡窗抢走（2026-11-14 三轮实证，坐标本身无误——PIL 像素实测行高 17px）→ `editor --selftest-outliner` 钩子走确定性路径：t>5s addEntity 两测试实体（真实 RPC）→ 树刷新后注入双节点多选态 → t>15s 批量删除测试实体（**不删 preview-\* 基线——删空场景 rebuild 拖死 viewer，2026-11-14 实证 crash-isolation**）→ t>20s Undo 一次树复原。机械判据 = Selected 高亮行像素带（ImGuiCol_Header 实测 RGB(31,57,88)，y547-558/564-576 两带）+ `selected: 2` 计数 + 批量按钮上屏。

---

## 11. C++ 引擎侧接口契约（`editor_bridge`，vendored、零运行时依赖）

```cpp
// editor_bridge/rpc_bridge.hpp  (示意，最终以实现为准)
namespace nl::editor {

struct RpcValue;                 // msgpack 载荷包装（内部用 msgpack::object）
struct RpcError { int code; std::string message; };
using RpcResult = std::variant<RpcValue, RpcError>;
using HandlerFn = std::function<RpcResult(const RpcValue& params)>;
using NotifyFn  = std::function<void(const RpcValue& params)>;

class RpcBridge {
public:
    // 生命周期（引擎侧为 client，连接编辑器 server）
    bool connect(const std::string& endpoint = "tcp://127.0.0.1:29110");
    void update();               // 每帧调用：收发包、分发、超时、心跳
    void shutdown();

    // 注册（对应 Sunshine 的 Register() 方法表 + _<uuid>_<method> 命名）
    void registerHandler(std::string_view method, HandlerFn);
    void registerNotify (std::string_view method, NotifyFn);

    // 出站（对应 Sunshine 的 Server/GameServer proxy）
    RpcResult call(std::string_view method, const RpcValue& params, int timeoutMs = 5000);
    void      notify(std::string_view method, const RpcValue& params);

    // 生命周期回调（对应 OnPluginReady / OnPluginClose）
    std::function<void()>                       onReady;
    std::function<void()>                       onClose;
    std::function<void(uint64_t hwnd,int pid)>  onAnnounce;  // 自身 announce 回调由编辑器侧消费

    // 配置
    void setHeartbeat(bool on, int timeoutMs = 6000);
};

} // namespace nl::editor
```

- 依赖：自研 `editor_bridge/msgpack_min.hpp`（header-only，零依赖）。（2026-11-13 勘误：原文写「`msgpack-c`（header-only，MIT，随 vendored 入库并附 `VENDOR.md`）」有误——msgpack-c 为 BSL-1.0 且 C++ 版默认依赖 boost，未 vendored。）
- `update()` 必须在引擎主循环每帧调用，否则入站调用与超时不会触发。

---

## 12. C++ 编辑器侧接口契约（Dear ImGui 宿主，与引擎侧同源 msgpack-c）

编辑器宿主为独立 C++ 程序，UI 用 Dear ImGui（MIT，header-only 风，随 vendored 入库），视口嵌入控件用 Win32 `SetParent`。RPC 实现与引擎侧**共用同一套 msgpack-c 编解码与信封结构**，仅角色反转（编辑器为 Server，引擎为 Client）。

```cpp
// editor/rpc_host.hpp  (示意，最终以实现为准)
namespace nl::editor::host {

struct RpcValue;                 // 与引擎侧同一 msgpack 包装
struct RpcError { int code; std::string message; };
using RpcResult = std::variant<RpcValue, RpcError>;
using HandlerFn = std::function<RpcResult(const RpcValue& params)>;
using NotifyFn  = std::function<void(const RpcValue& params)>;

class RpcHost {
public:
    // 生命周期（编辑器侧为 server，监听引擎 client）
    bool start(const std::string& endpoint = "tcp://127.0.0.1:29110");
    void update();               // 由宿主消息循环每帧调用：收发、分发、超时、心跳
    void stop();

    // 注册（引擎→编辑器调用，如 editor.log / editor.sceneChanged）
    void registerHandler(std::string_view method, HandlerFn);
    void registerNotify (std::string_view method, NotifyFn);

    // 出站（编辑器→引擎命令，如 engine.setCamera）
    RpcResult call(std::string_view method, const RpcValue& params, int timeoutMs = 5000);
    void      notify(std::string_view method, const RpcValue& params);

    // 生命周期回调
    std::function<void(uint64_t hwnd,int pid,int proto,const RpcValue& caps)> onEngineAnnounce;
    std::function<void()> onEngineReady;
    std::function<void()> onEngineClose;   // 掉线 → 显示空视口 + 自动重连
};

} // namespace nl::editor::host
```

- 依赖：自研 `editor_bridge/msgpack_min.hpp`（header-only，零依赖，与引擎侧同一份）。（2026-11-13 勘误：原文写「`msgpack-c`（header-only，MIT）」有误——msgpack-c 为 BSL-1.0 且 C++ 版默认依赖 boost，未 vendored。）
- 嵌入：`onEngineAnnounce` 收到 `hwnd` 后，调用 Win32 `SetParent((HWND)hwnd, viewportFrameHWND)` 并裁剪/随尺寸同步；借鉴 Sunshine `GameWidget.py` 的几何同步逻辑，但用 C++/Win32 重写。
- 泵驱动：`RpcHost::update()` 由宿主窗口的消息循环（ImGui 主循环或独立 IO 线程）每帧调用，确保入站调用与超时及时触发。
- 插件体系（不在本规范范围，但编辑器侧约定）：各工具插件经统一扩展点注册自己的 `registerHandler`/`registerNotify` 与 ImGui 面板绘制函数，彼此只经 RPC 与宿主服务交互，禁止插件间直接依赖——对应 Sunshine 的扩展点/服务隔离原则。

---

## 13. 版本与兼容

- 信封 `jsonrpc` 字段标记协议主版本（当前 `1`）。
- 握手时双方交换 `rpc.version`，主版本不一致 → 返回错误码 `200` 并断开。
- 副版本向后兼容：新增可选字段不破坏旧端；新增方法不要求旧端实现（未实现走 `100`）。

---

## 14. 安全与边界

- **仅本地**：客户端绑定 `127.0.0.1`，不暴露到网卡；命名管道走本地命名空间。
- **单客户端**：同一端口仅接受一个引擎连接；重复连接拒绝（返回错误并保留首个）。
- **载荷上限**：8 MiB，防滥用。
- 不传输帧缓冲 / 大块资源；大资源走文件系统或引擎内已有资产管线。

---

## 15. 已拍板 / 开放项（M-E6 收口）

- [x] **端口固定 `29110`**；两端用 `--rpc-endpoint` / `--editor-endpoint` 显式指定同一 endpoint，不做随机端口 + 通知文件。
- [x] **TCP 为默认，命名管道为可选**（Windows 专属），不做命名管道默认。M-E6 已实装 `pipe://<name>`，CLI 显式切换。
- [x] **`SetParent` 嵌入由编辑器主动挂**（Sunshine 方式）；引擎只上报自身 HWND，不带 `--parent-hwnd`，对启动顺序更鲁棒。
- [x] **多视口 = 一个编辑器同时接纳多个引擎进程**，按 `viewId` 路由并各自嵌入独立视口面板；`viewId` 缺省 `main` 保持老插件兼容。M-E6 已实装。
- [x] **单进程多 HWND / 多 swapchain 明确否决**，不进本轨道范围（避免把「多视口」误解成引擎侧多渲染目标）。

> 开工闸口径：M-E6 开工前待拍板项 4 条已全部拍板并写回本表，冻结面（Q4 GBuffer / `renderer/` / `shader/` / `example/` / `tools/regression/cases.json`）零触碰。
