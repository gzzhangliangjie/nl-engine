# B4 — 热重载（改图即重编译）

> 轨道：B 路材质图 → Slang（PARALLEL-TRACKS.md §2.1），A 类并行轨道，走工具版交代，不进渲染总闸。
> 状态：**已完成并验收**（`feat/material-graph` 分支，本地）。

## 目标
让「改图 → 重新编译双后端」自动化：外部编辑器改 `.mg` 文件后，工具自动检测变更并重新 emit + `slangc` 编译 SPIR-V/DXIL × vs/ps，实时回报 OK/FAIL。

## 设计
1. **复用 B3 链路**：把 `check` 里的 emit + 4 路 `slangc` 抽成 `recompileGraph(graph, slangc, dxc, RecompileReport*)`，返回逐目标结果。`check` 与 `watch` 都走它（单一验收口径，无重复实现）。
2. **`watch <in.mg>` 轮询循环**：
   - `std::filesystem::last_write_time` 轮询 mtime（默认 200ms 间隔，可调 `--interval`）。
   - 首轮先编译一次作基线；后续 mtime 变化即重新 `recompileGraph`。
   - `--once`：只编译一次后退出（exit code = 重编译结果），用于 ctest 非交互验收。
   - `--slang <DIR>` / `--dxc <DIR>` 覆盖 `NL_SLANG_BIN` / `NL_DXC_DIR` 默认定位。
   - `std::setvbuf(stdout, _IONBF)`：长进程走管道默认全缓冲，强制无缓冲以便实时看到每次重编译结果（否则循环期间看不到输出，直到退出）。
3. **逐目标回报**：`RecompileReport{psSpirv, vsSpirv, psDxil, vsDxil}`，FAIL 时打印各目标布尔，便于定位是哪个后端/入口编译挂了。

## 参考口径
- 复用 B3 的 `model.slang` 绑定约定（命名入口 vsMain/psMain、register(b0/t0/s0)、cbuffer SceneConstants）。
- 与现有 `nanite-tools` 工具纪律一致：零图形 API，只依赖 core 日志。

## 验收
- `ctest b4-graph-hot-reload`：`watch sample.mg --once` 单次重编译双后端四入口全 OK（PASS_REGULAR_EXPRESSION "OK"），DEPENDS b3。
- 实测热重载循环：后台跑 `watch`，外部 `build` 重写 `.mg` → 日志出现第二次 `HH:MM:SS OK`，证明确实检测到变更并重编译。
- `ctest b2+b3+b4` 全绿（工具版交代，不进渲染回归总闸 bytes/near）。

## 与后续里程碑关系
- B5（S7 之后）接 PBR 材质 pass：把 `recompileGraph` 生成的 `.slang` 接进现有 PBR 光照，热重载即可实时换材质。
- 节点库扩展（每类一个 emitter 分支 + 单测）对热重载透明：新增节点只改 `emit.cpp`，`recompileGraph` 不变。
