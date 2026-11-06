# S13 详细开发路线图：簇级 BLAS 聚合 + 延迟一帧回读

> 对应总路线 `docs/ROADMAP.md` 的 S13。预计 3 个里程碑、业余时间 1~1.5 周。**接缝层：全路线的标志性难题。**
> 此前光追用的是 fallback mesh（全局低模）——GI 反射的几何和屏幕上渲染的几何对不上，LOD 切换处会闪。解法：**可见簇列表回读（延迟一帧）→ 拷贝预建 per-cluster BLAS 聚合成当帧 BLAS → TLAS**。Carrot《Raytracing》是此题唯一的公开详解。

---

## M13.0 per-cluster BLAS 预建 + 内存池（2 天）

- [ ] 离线/启动时：每个簇用**自身几何**预建一个小 BLAS，全部常驻；建索引映射 clusterId → BLAS
- [ ] BLAS 池化：聚合 BLAS 的实例缓冲/缓冲区复用，杜绝每帧分配

**自检**：全部簇 BLAS 构建成功；内存占用记录在案（HUD）。tag：`s13-preblas`。

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
