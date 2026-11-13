# S13 详细开发路线图：簇级 BLAS 聚合 + 延迟一帧回读

> 对应总路线 `docs/ROADMAP.md` 的 S13。预计 3 个里程碑、业余时间 1~1.5 周。**接缝层：全路线的标志性难题。**
> 此前光追用的是 fallback mesh（全局低模）——GI 反射的几何和屏幕上渲染的几何对不上，LOD 切换处会闪。解法：**可见簇列表回读（延迟一帧）→ 拷贝预建 per-cluster BLAS 聚合成当帧 BLAS → TLAS**。Carrot《Raytracing》是此题唯一的公开详解。

---

## M13.0 per-cluster BLAS 预建 + 内存池（2 天）✅ 已收官 2026-11-13（tag `s13-preblas`）

- [x] 离线/启动时：每个簇用**自身几何**预建一个小 BLAS，全部常驻；建索引映射 clusterId → BLAS
  - 落点：`Scene::initClusterBlas`（initDag 拼接循环完成后调用，`--cluster-blas` 默认关）。全部簇共享两块 build-input slab（position-only 12B/顶点 VB + u8 局部索引→u32 展开 IB——簇顶点在 .nmesh 已去重，「重展开」即索引格式转换，无需顶点重排）；每簇 geometry 用 `GeometryTriangles.vertexOffset/indexOffset` 引用窗口；`clusterBlas_` 下标 = 全局簇 id（层间连续拼接，与 clusterMeta 同序）
  - 构建分块提交（2048 build/列表 + 块间 waitForIdle）：单列表 43k 次 build 超 DX12 TDR 2s → `0x887a0005 DEVICE_REMOVED` 间歇杀进程（实锤后分块修复，三连跑零复发）；permanent 态必须首块落定（非永久 setBufferState 只活在本列表 → 后续块 Unknown prior state → GPU hazard，第二轮实锤）
- [x] 内存记账双口径（NVRHI `IAccelStruct` 无尺寸查询，真实值拿不到）：`inputBytes`（slab 实际字节，确定值）+ `estBytes`（上界估算 tri×64B + 512B/BLAS）；日志锚 + HUD 两行（`CLAS : N blas N tri` / `CLAS mem: input X MB est ~Y MB`）。实例/缓冲复用池化随 M13.1 聚合形态一并落（本里程碑先把 slab 共享做掉 = 对象数可控的关键）
- 实测：Suzanne 链 324 blas / 26ms；OcclScene1M **43547 blas / 2.6M tri / est ~181MB**（万级规模坑注记实测值在案：est 而非爆显存，裁粗层优化暂不需要）

**自检**：全部簇 BLAS 构建成功；内存占用记录在案（HUD）。tag：`s13-preblas`。✅（run.bat verify 全绿 + regression `--only s13-preblas` 4/4 bytes + 保形 `--only s8.0/s8.1` 10/10 bytes + `server.py --check` 0）

**三处实读结论（2026-11-13）**：Carrot = per-group 惰性 BLAS 缓存 + readback 下帧 CPU 消费（对象粒度粗一层，簇几何自身建 BLAS/常驻/按 id 索引同构）；bevy_solari = per-mesh 常驻 + compaction 队列；UE Nanite RT = per-resource BLAS + `r.RayTracing.Nanite.LODBias` 选层 + AuxiliaryData 命中回簇 + 64MB BLAS 缓存池。**无一家做 per-cluster 万级 BLAS**；S13-PLAN 拍板的每簇预建照落（est ~181MB 实测可承受），聚合形态见 M13.1 拍板项。

## M13.1 回读 + 聚合 + TLAS 重建（2~3 天，灵魂里程碑）⚠️ 开工闸：聚合形态待拍板

> **待拍板（2026-11-13 M13.0 实读发现）**：计划原文「拷贝/引用进当帧聚合 BLAS（NVRHI AS clone/compact）」在 NVRHI 抽象下**不可行**——`rt::GeometryDesc` 只有 Triangles/AABBs 两型（nvrhi.h:1568），无 Instances 型；BLAS 内引用子 BLAS 是 Vulkan 原生扩展语义，DX12 无此路径，NVRHI 不暴露。候选方案：**A = TLAS 直挂可见簇 BLAS 多实例**（回读列表 → 当帧 TLAS 每实例一簇，hit shader 用 instanceID=全局簇 id 反查几何；RTXMG 同构，每帧 TLAS rebuild 已是 M8.1 UE 口径，实例数 = 可见簇数千级）vs **B = 回退 fallback+簇混合**（可见簇区域用预建 BLAS，其余打 fallback BLAS，TLAS 实例数恒 2）。代价矩阵开工时补齐交用户拍板，拍板前冻结 M13.1 落码。

⚠️ 常见坑：
- 簇几何是**去重的局部索引**，BLAS 输入需要重展开成完整三角形——格式转换工具函数放 nanite-tools 或共享库
- BLAS 数量巨大（万级）：单卡内存吃紧的话，只对「误差敏感层级」的簇建 BLAS（LOD 高层粗簇跳过），先跑通再优化

## M13.1 回读 + 聚合 + TLAS 重建（2~3 天，灵魂里程碑）

- [ ] S5 遍历的可见簇列表**延迟一帧回读**（readback buffer 双缓冲，绝不同帧等待）
- [ ] 每帧按回读列表：把对应预建 BLAS **拷贝/引用**进当帧聚合 BLAS（NVRHI AS clone/compact 相关 API）→ 建 TLAS
- [ ] S8 的 `--mode rt` 切到聚合 BLAS 上验证：光追几何 = 当前 cut 几何

**自检**：近看模型，调试射线命中的是精细簇（不再是 fallback 粗模）；帧时间无尖刺。tag：`s13-merge`。

⚠️ 常见坑：
- 延迟一帧的代价 = 相机快速移动时射线几何滞后一帧——表现为 GI/反射边缘轻微滞后，可接受（UE 同样做法）
- 回读列表与当帧 cut 不一致（一帧延迟的本质）会造成 AS 与画面短暂不一致：**不要**试图同帧回读修它，会引入 CPU-GPU stall，得不偿失

## M13.2 接缝验收（1 天）

- [ ] `--mode final`：GI/反射用当前 cut 几何
- [ ] LOD 切换压力测试：相机匀速拉远/拉近，录制屏

**自检（S13 验收）**：LOD 切换处 GI/反射**无跳变闪烁**；时间线截图（Nsight/RGP）证明无 CPU-GPU 同步 stall。tag：`s13-done`，录屏 `docs/screenshots/s13_*.png`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M13.0 预建 BLAS+池 | 2 天 | `s13-preblas` |
| M13.1 回读+聚合 | 2~3 天 | `s13-merge` |
| M13.2 接缝验收 | 1 天 | `s13-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| **此题唯一公开详解** | Carrot《Recreating Nanite: Raytracing》（读三遍不为过） |
| 管线定义实例 | `Carrot/engine/resources/pipelines/compute/compute-rt-clusters.json`（逐簇 BLAS 聚合管线定义，连管线文件都留着） |
| 官方级参考（卡壳超 3 天） | NVIDIA **RTXMG**——簇式几何加速 BVH 构建的 MIT 官方实现 |
| 方案对比 | kb/index.html#bridge（03 章：fallback / 逐簇聚合 / 自建 BVH 三方案对比 + Carrot 每帧五步） |

## 底线

- **绝不同帧回读**——这条底线破了，前面所有「无 stall」的努力报废
- 聚合 BLAS 的内存池从第一天做，万级簇每帧分配 = 帧率崩溃 + 显存碎片双杀
