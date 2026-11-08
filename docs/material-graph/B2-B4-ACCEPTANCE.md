# 材质图轨道 B2~B4 验收纪要（固化文档闸，merge 回 master 前置条件）

> 日期：2026-11-08　分支：`feat/material-graph`　worktree：`E:/UE_STUDY_2026/nl-engine-material-graph`
> 轨道性质：A 类并行轨道（非渲染 / 引擎化），**不碰 `kStage`、不进渲染回归总闸**，走工具版交代。
> 参考：三处已定向读——PhasmaEngine MaterialX→Slang 适配（最贴样本）、Slang 官方 `SlangShaderGenerator`（vertex/pixel 分段语义）、UE `MaterialEditor`（结构只读）；bevy 无材质图等价物，据实标注。

## 一、目标达成（B 路阶梯 §2.1）

| 步 | 目标 | 状态 | commit |
|---|---|---|---|
| B2 | 节点图数据结构 + 序列化 | ✅ | `1a912fa` |
| B3 | 图 → Slang 代码生成 + 双后端编译验收 | ✅ | `0cb2593` |
| B4 | 热重载（改图即重编译） | ✅ | `b01959e` |
| B5 | 接 PBR 材质 pass | ⏳ S7 之后 | — |
| B1 | 固定管线材质常量 | 由现有 `model.slang` 的 `SceneConstants` 承担 | — |

## 二、验收判据与证据

**判据**：A 类工具版交代——`ctest` 全绿 + 证据图必须从工具真实输出生成（不许手绘）。

1. **B2**：`material-graph-tools selftest` PASS——构造/validate（引脚存在、输入单入边、类型一致）、序列化确定性（两次写出自节一致）、roundtrip 字节一致、三类负向（类型不匹配连线 / 重复入边 / 缺 header）全过。ctest `b2-graph-selftest`。
2. **B3**：生成的 `.slang` 经 **slangc 真实编译**：SPIR-V + DXIL 双后端 × vs/ps 四入口全 exit 0（`genslang: OK`）。生成产物符合 `model.slang` 约定：命名入口 `vsMain`/`psMain`、`register(b0/t0/s0)`、`cbuffer SceneConstants`。ctest `b3-graph-genslang`。
3. **B4**：`watch --once` 单次重编译 PASS（ctest `b4-graph-hot-reload`）；另做实测——后台跑 `watch` 轮询，外部重写 `.mg` → 日志出现第二次 `OK`，证明确实检测变更并重编译。
4. **证据图**：`material-graph-tools render <in.mg> <out.svg>` 从 `.mg` 真实数据确定性绘制 SVG（节点框 / ValueType 着色引脚 / 贝塞尔连线 / 属性值），Edge 无头截图成 `docs/material-graph/b3-nodegraph.{svg,png}`——4 节点 3 连线（2×ConstantFloat → Multiply → Output），布局与连线正确，value 文本在框内。
5. **主干同步后复验**：master（`8faad52`，S4 领先 4 提交）合入后 merge commit `b2d1050`，重配 + 重编 + ctest **3/3 全绿**。

## 三、新增约定 / 不变量（写回 canonical）

- **material_graph v1 文本格式**：无依赖、确定性序列化；`Node.x/y` 是编辑器布局坐标、无渲染语义，`render` 直接信任它。
- **A 类证据图铁律落地**：CLI 工具不产 GPU 截图时，证据图 = 从工具真实输出（`.mg`）生成的 SVG→PNG，禁止手绘示意。
- **emitter 口径**：纯函数、确定性、零依赖；从 Output 节点向上游 DAG 递归求值；生成 `.slang` 必须符合项目绑定约定（`-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`，shader 只写 `register(b0/t0/s0)`）。
- **check/watch 同源**：都走 `recompileGraph()`（emit + slangc 双后端 ×4），单一验收口径不重复实现。

## 四、踩坑留档

1. **junction 共享 `.workbuddy` 挡 merge**：skip-worktree 状态下本地未提交的记忆文件改动，merge 拒绝覆盖，且 `stash` 也看不见。解法：物理备份 → 临时 `--no-skip-worktree` + `checkout --` 还原索引版本 → merge → `cp` 恢复 → 重设 skip-worktree。
2. **`reg.exe` 被沙箱拉黑** → `vcvarsall` 定位不到 Windows SDK（rc.exe/include/lib 缺失）。解法：手动注入 SDK 路径进构建批处理；批处理内**禁中文注释**（非 UTF-8 代码页下被 cmd 拆行）。
3. **匿名命名空间里的 `main` 无外部链接** → 链接期找不到入口；`main` 须在全局作用域（与 nanite-tools 一致）。
4. **`system()` 引号陷阱**：可执行路径无条件加引号会让 cmd 解析失败；只在路径含空格时加引号。
5. **长进程管道全缓冲**：`watch` 走管道时 stdout 全缓冲，实时输出不可见；`setvbuf(stdout, nullptr, _IONBF, 0)` 修复。
6. **render 属性行溢出**：节点高度初版只算引脚行没算 props 行，value 文本画到框外；抽统一 `nodeHeight()`（引脚行 + propN×14）修复，画布尺寸与节点框同一口径。

## 五、merge 回 master 前置自查（假绿防护）

- [x] 轨道内 `git status` 干净（本批改动全部已提交）；
- [x] ctest b2/b3/b4 全绿（主干同步后复验 3/3）；
- [x] 本验收纪要 + `MILESTONES.md` 四栏 + `PARALLEL-TRACKS.md` 登记表更新，全部在轨道分支提交；
- [ ] 主 worktree 侧 `git status` 无本批改动（合并时复核）；
- [ ] merge 后主干重配 + 编 + `regression.py` 总闸复核（渲染资产未动，预期全绿）。
