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

> 偏离理由：① 编辑器宿主从 Python/PyQt5 改为 **C++ + Dear ImGui**——纯 C++（MIT，免费，零运行时依赖）与引擎共享类型，契合本项目 vendored 习惯；Sunshine 的插件体系/嵌入控件仅作架构思想借鉴，不复用其 Python 代码。② 线协议同理：`simplerpc` 是 Python 库无法在 C++ 侧复用；两侧统一用 **`msgpack-c`（header-only MIT，vendored 入库）**，序列化与引擎侧同源。JSON-RPC 2.0 的 `method/params/id/result/error` 结构对调试友好，保留语义、替换编码。

---

## 3. 传输层

- **默认传输**：本地 TCP，`127.0.0.1`，默认端口 `29110`（避 Sunshine 的 29102）。可通过 `--rpc-port` 覆盖。
- **备选传输**：Windows 命名管道 `\\.\pipe\nl-editor-rpc`（更低延迟、无端口冲突）；两类传输**共用同一套上层协议**，仅 `endpoint` 字符串不同（`tcp://127.0.0.1:29110` 与 `pipe://nl-editor-rpc`）。
- **连接模型**：**编辑器为 TCP Server（监听），引擎为 Client（主动连接）**——对齐 Sunshine（引擎侧 `SDK_CLIENT` 连回编辑器 `Sunshine` server）。
- **字节序**：所有多字节整型小端（LE）。

---

## 4. 分帧（Framing）

每条消息 = `4 字节 LE uint32 长度 N` + `N 字节 msgpack 编码的载荷`。

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

- 依赖：`msgpack-c`（header-only，MIT，随 vendored 入库并附 `VENDOR.md`，符合选型硬闸）。
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

- 依赖：`msgpack-c`（header-only，MIT，与引擎侧同一份 vendored 副本，符合选型硬闸）。
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

## 15. 待拍板 / 开放项

- [ ] 端口 `29110` 是否固定，还是每次启动随机 + 通过命令行/文件通知引擎？
- [ ] 命名管道是否作为默认（Windows 专属），还是 TCP 默认、管道可选？
- [ ] `SetParent` 嵌入由**编辑器主动挂**（Sunshine 方式）还是引擎启动时带 `--parent-hwnd` 自建子窗口？（推荐前者，对启动顺序更鲁棒）
- [ ] 是否需要「多视口」（同一引擎多个嵌入窗口）——影响 `engine.announce` 的 hwnd 结构（当前为单 hwnd）。
