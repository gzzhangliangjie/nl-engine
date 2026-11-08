# .nmesh 分页友好格式 · 字段级草案（已拍板，待 V1 落地）

> 状态：**DECIDED / 2026-11-08 用户拍板「5 个点全按推荐来」**。字段布局已冻结，仅等 V1 实现时落到 `meshlet_io.cpp`。
> 依据：P9-PLAN 全局不变量 INV-1/2/3；用户 2026-11-08 拍板「提前预留流式接口」。
> 铁律：`AGENTS.md` ④——触碰 `.nmesh` 资产格式须先摆三处证据 + 历史约定，再请用户定（证据见 §3，已满足）。
> 落地时机：见 §8。代码改动**不在现在**——S0~S14 回归基线（M3.2 已 150/150 ALL GREEN）不因此草案破坏。

---

## 1. 为什么改（问题陈述）

当前 `.nmesh`（v1/2/3）的 `vertices` 是**跨所有 meshlet 共享的单一数组**：每个 meshlet 用 `vertexOffset/vertexCount` 指向这块共享池的一个窗口。

- 这满足 `kb/index.html:475` 警告的**反模式**："簇共享顶点池会让按簇加载 / 流式变得很麻烦"。
- INV-1 要求："顶点按簇自包含、可分页存储，绝不为全局共享顶点池焊死结构。"
- V4 流式要能"按页加载 / 卸载一页、且不引用页外顶点"，当前布局做不到。

**目标**：在 v4 把顶点组织成**页对齐、顶点闭合（vertex-closed）的 slab**，使任意一页的数据自包含、可独立加载。

---

## 2. 当前格式精确速览（改之前）

```
NMeshHeader            20B  magic=NMSH, version{1,2,3}, nameLen, meshletCount, vertexCount, triangleCount
name                   nameLen B
NMeshletRecord[]       meshletCount × 52B   // vertexOffset/vertexCount/triangleOffset(BYTE)/triangleCount + sphere[4]/coneAxis[3]/coneCutoff/error
MeshVertex[]           vertexCount × 32B    // pos[3]+normal[3]+uv[2]，直接内存映像
uint8[]                triangleCount × 3    // 每 meshlet 局部索引 0..63
[v2+] NMeshMetaHeader  24B  magic=NLOD  + payload(锁点 [+ DAG 段])
[v3]  NMeshFallbackHeader 24B  magic=NFLB + payload(回退_meshVertex[] + u32 index[])
```

关键：**`vertices` 一个数组，meshlet 偏移绝对指向它**；页 / 组概念不在格式里（DAG 血缘在 NLOD 段，但顶点仍是共享池）。

---

## 3. 三处证据（支撑"自包含 + 偏移寻址"）

| 来源 | 实况 | 给我们 |
|---|---|---|
| **bevy** `from_mesh.rs:206` `build_and_compress_per_meshlet_vertex_data(meshlets.get(i).vertices,…)` | 逐 meshlet 拷贝并压缩顶点；提交 `aa626e4f0b` Objective 第一行即 "Prepare for streaming" | "顶点自包含" 有现成同代实证；流式是预埋动机 |
| **UE5** `NaniteStreamOut.h` `FStreamOutMeshDataHeader` = `VertexBufferOffset / NumVertices / IndexBufferOffset / NumIndices` | 流式 = 把一页连续 slab 拷进 **同一块大 GPU buffer 的固定偏移**，meshlet 偏移永不改 | **推荐模型的终态参照**：大 buffer + 每页 offset/count，不 patch 偏移 |
| **Carrot** `ClusterManager.cpp:144-150` | 每簇 `vertices.resize(firstVertexIndex+meshlet.vertexCount)` 独立拷贝 | 逐簇自包含是共识；但 Carrot 无流式模块 → 须我们自己做（无法照搬） |
| **kb** `index.html:475` / `:820` | 475 警告全局共享顶点池；820 点名 `NaniteStreamingManager*.cpp` = 分页与驻留策略 | 避开后期返工点；V4 直接对标 UE5 驻留管理器 |

**三处一致结论**：顶点自包含 + 偏移寻址大 buffer + 独立 page/residency 管理 = 流式正路；`kb:475` 的共享池三处来源**无一采用**。

---

## 4. 推荐设计：v4 = v3 + 可选 `NMeshPageTable` 尾段（UE5 风格）

### 4.1 核心思路（最小破坏、最贴合现有 GPU buffer 模型）

- **meshlet 记录布局完全不变**：`vertexOffset/vertexCount/triangleOffset/triangleCount` 仍**绝对**指向共享 `vertices` / `triangles` 数组（与 v1~v3 同款，磁盘记录零改动）。
- 离线新增一步 **page-builder pass**：把 `vertices` / `triangles` **重排成页连续的块**，并保证每页**顶点闭合**（页内 meshlet 只引用本页顶点；跨页共享的顶点在边界处**复制**，bevy/Carrot 同款做法）。
- 新增一个**页表尾段**，记录每页的 `[firstMeshlet, meshletCount)` 与其顶点/索引 slab 的 `[offset, count)`。
- **向后兼容**：版本升到 4；v1~v3 reader 按既有规则拒 v4（老 build 本来就读不了新版本，符合"version 递增"约定）。当前 build 读 1/2/3/4。

### 4.2 新增字段（全部 static_assert 锁死布局）

```c
// 页表尾段头，24B，镜像 NMeshMetaHeader 尺寸，reader 复用解析骨架
// （注：草案初版误写成 20B，已按 static_assert(==24) 修正为下 layout）
struct NMeshPageTableHeader {
    std::uint32_t magic;         // 'N','P','A','G' → 0x4741504E (LE)
    std::uint32_t payloadBytes;  // = pageCount * sizeof(NMeshPageRecord)
    std::uint32_t pageCount;     // 页数（u32，避 16-bit 上限）
    std::uint16_t version;       // 1
    std::uint16_t reserved0;     // 0
    std::uint32_t reserved1;     // 0
    std::uint32_t reserved2;     // 0
};
static_assert(sizeof(NMeshPageTableHeader) == 24, "page table header drift");

// 每页 32B，镜像 NMeshDagGroupRecord 尺寸
struct NMeshPageRecord {
    std::uint32_t firstMeshlet;   // 本页首 meshlet 号
    std::uint32_t meshletCount;   // 本页 meshlet 数
    std::uint32_t vertexOffset;   // 顶点 slab 基（绝对，= 磁盘 & GPU 同一偏移）
    std::uint32_t vertexCount;    // 顶点 slab 长度
    std::uint32_t triangleOffset; // 索引 slab 基（BYTE 偏移，绝对）
    std::uint32_t triangleCount;  // 索引 slab 长度（三角数）
    std::uint32_t flags;          // bit0 = 本页含跨页复制的边界顶点
    std::uint32_t reserved;       // 0
};
static_assert(sizeof(NMeshPageRecord) == 32, "page record drift");
```

尾段布局：`NMeshPageTableHeader` + `NMeshPageRecord[pageCount]`。由 `loadNMesh` 在 v4 分支按 magic `NPAG` 识别（与 NLOD/NFLB 同级，顺序不敏感、重复段拒绝）。

### 4.3 页不变量（离线 builder 保证 + loader 校验）

对每页 P，其所有 meshlet `i ∈ [firstMeshlet, firstMeshlet+meshletCount)`：
- 顶点窗口 `[m.vertexOffset, m.vertexOffset+m.vertexCount)` ⊆ `[P.vertexOffset, P.vertexOffset+P.vertexCount)`
- 索引窗口 `[m.triangleOffset, m.triangleOffset+m.triangleCount*3)` ⊆ `[P.triangleOffset, P.triangleOffset+P.triangleCount*3)`
- **顶点闭合**：不同页的 meshlet 不引用同一顶点，除非该顶点被复制（flags.bit0 标记）。

> 注：因 `triangles` 是每 meshlet 局部索引（0..63）且 meshlet 窗口不重叠，索引闭合天然成立；顶点闭合需 page-builder 重排保证。

### 4.4 流式（V4）如何消费

- GPU 端维护**一块大 vertex/index buffer**（尺寸 = 整资产），与磁盘偏移一一对应。
- 驻留 = "某页 slab 已拷入 GPU buffer 的对应偏移"；绘制前 `ensureResident(P)`，未驻留则异步拷入。
- **meshlet 偏移永不 patch**（同 UE5：固定偏移拷入，偏移天然对得上）。
- 淘汰：LRU 逐页回收（重新按需拷入即可，无需回写）。

### 4.5 页分区来源（已拍板：复用 S3 DAG 组）

S3 已把 meshlet 按 `dagGroups`/`clusterGroup` 分组（按组重切、父簇不跨组）。**page-builder 以 DAG 组为种子**：

- **单组预算 ≤ 128 KiB（默认预算）**：整组作为一页。
- **单组 > 128 KiB**：按预算二分切分（不跨 meshlet 切，保证组→页映射仍整数个 meshlet）。
- **相邻多个小组 < 预算**：合并为一页。
- 跨页共享顶点在边界复制（满足 §4.3 顶点闭合）。

→ **不重造分区逻辑，复用已验证的 S3 结构**；预算默认 128 KiB，实现期允许 64–256 KiB 范围按资产规模调。

---

## 5. 备选方案（bevy 风格，列出供对比，不推荐首选）

| 维度 | 推荐（UE5 风格 / 偏移大 buffer） | 备选（bevy 风格 / 逐 meshlet 内联） |
|---|---|---|
| 顶点存储 | 重排后的共享数组 + 页表 | 每 meshlet 顶点内联进其 record |
| 磁盘布局变动 | 仅加页表尾段；meshlet 记录不变 | meshlet 记录变长、无共享数组 |
| 偏移语义 | 绝对，流式零 patch | 每 meshlet 自带顶点，天然自包含但 reader 大改 |
| 内存开销 | 仅边界顶点复制 | 组内共享顶点也复制，开销更大 |
| 贴合现状 | 高（GPU buffer 模型不变） | 低（需改渲染上传路径） |

→ **推荐 UE5 风格**：破坏性最小、复用 DAG 组、贴合现有 GPU 上传，且直接对标工业终态。

---

## 6. 渲染接口预埋（INV-3）

```c
// renderer 侧抽象（V1~V3 实现为全常驻，V4 替换为分页）
struct StreamingProvider {
    virtual void ensureResident(uint32_t pageId) = 0; // V1~V3: no-op
    virtual bool isResident(uint32_t pageId) const = 0; // V1~V3: always true
};
// AllResidentProvider（V1~V3） / PagedProvider（V4）
```

V1~V3 的 `viewer` 用 `AllResidentProvider`：启动即全量上传，页表被忽略。V4 换 `PagedProvider`。**接口签名现在定，实现后换**（INV-3）。

---

## 7. 已拍板决策点（2026-11-08，用户「全按推荐来」）

| # | 决策点 | 拍板值 |
|---|---|---|
| 1 | 分页粒度 | **按字节预算细分**，默认 **128 KiB**（范围 64–256 KiB 可调）；DAG 组整体 ≤ 预算则整组一页，超预算二分，相邻小组并页（见 §4.5） |
| 2 | 边界顶点复制 | **接受**（bevy/Carrot 同款，少量内存开销换顶点闭合） |
| 3 | 版本与时机 | **V1 即写 v4**（提前预埋 `NPAG` 页表，符合「提前预留流式接口」） |
| 4 | 页表必备性 | **v4 强制携带 `NPAG` 段**；loader 发现 v4 但缺 `NPAG` 即拒绝（非法文件） |
| 5 | 页分区种子 | **复用 S3 `dagGroups`**（不重造分区逻辑，单组 > 256 KiB 才二分） |

> 五个点已全部锁定，字段布局冻结。实现落到 `meshlet_io.cpp` 的时机见 §8。

---

## 8. 落地时机与下一步（不破坏现有管线）

**现在不落地代码**，原因：
- V1 属于 P9（S0~S14 + P8 之后的收尾阶段），当前主线尚未走到。
- 现在改 `meshlet_io.cpp` 要么改默认输出（打破 M3.2 已 150/150 ALL GREEN 的回归基线，所有 v3 文件失效），要么加未经验证的 flag——都没有真实资产可测，风险无意义。
- 铁律要求 `.nmesh` 格式变更「摆证据 + 用户确认后再落地」，证据已齐（§3）、用户已拍板（§7），但**触发时机应在 V1 启动、真实资产到位前**。

**V1 启动时的落地步骤（届时照敲）**：
1. `meshlet_io.hpp`：加 `NMeshPageTableHeader` / `NMeshPageRecord`（`static_assert` 锁 24B / 32B）；`NMeshVersion` 加 `v4`。
2. 离线 `page-builder` pass（落在 `nanite-tools lod`，复用 S3 `dagGroups` 作种子，按 §4.5 预算切分 + 边界顶点复制）。
3. `saveNMesh` 写 v4 + 强制 `NPAG`；`loadNMesh` v4 分支按 magic 识别并校验 §4.3 页不变量。
4. 渲染侧 `StreamingProvider` 抽象（§6）入 `renderer/` 接口头；V1~V3 用 `AllResidentProvider`（no-op），V4 换 `PagedProvider`。
5. 回归：先保证 v1~v3 读取**完全不受影响**（既有基线零改动），再在 Suzanne 上跑通 v4 写出→读回→双后端 bytes/near，作为 V1 验收用例入 `example/cases.json`。

**若现在就想看代码雏形**：可提前做第 1 步（加结构定义，纯声明、不改默认行为）作为预埋——你点头我就动 `meshlet_io.hpp` 的声明部分。
