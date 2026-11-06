# S4 详细开发路线图：硬件光栅 + GPU 视锥剔除 + Indirect Draw

> 对应总路线 `docs/ROADMAP.md` 的 S4。预计 3 个里程碑、业余时间 1~1.5 周。
> 从本步起进入 **GPU-driven**：CPU 不再逐簇提交，而是 compute 剔除 → 写 indirect 命令 → 一次 ExecuteIndirect 画全部可见簇。这是后面 S5/S6 的骨架。

---

## M4.0 簇数据上 GPU + indirect 封装（1~2 天）

- [ ] .nmesh 全部簇 + 顶点/索引一次性上显存（S0 的 staging 封装直接复用）
- [ ] 渲染簇的 draw 命令结构：`DrawIndexedIndirectCommand{indexCount, instanceCount=1, indexOffset, vertexOffset}` per cluster
- [ ] NVRHI indirect：ICommandList `drawIndexedIndirect`，indirect buffer 可写（compute 可见）
- [ ] 先不做剔除：instanceCount 恒 1，全簇画出来（等价 S0 的普通画法，作为回归基线）

**自检**：画面与 `--mode normal` 逐像素一致（对照截图）——证明 indirect 通路本身没画错。tag：`s4-indirect`。

## M4.1 compute 视锥剔除（1~2 天）

- [ ] 每帧 compute pass：一次 dispatch 全簇并行，每线程处理 1 簇——球体 vs 视锥 6 平面测试
- [ ] 通过 → 写 indirect 命令 + 可见簇计数（atomicAdd）；不通过 → instanceCount=0
- [ ] 注意 indirect buffer 用带计数变体（executed count 由 GPU 写，draw count 上限 = 簇总数）

**自检**：相机背对场景时 draw count ≈ 0；转回来恢复全量。tag：`s4-cull`。

⚠️ 常见坑：
- 视锥平面从**视图投影矩阵**提取（Gribb-Hartmann 行抽取），别对错行列
- compute 写 indirect 和 graphics 读 indirect 之间要 barrier——NVRHI 自动跟踪通常覆盖，但**显式 UAV→IndirectArg 状态**在 DX12 后端要确认（Donut 示例有现成写法）

## M4.2 HUD 统计 + 性能验收（1 天）

- [ ] 剔除统计回读：draw count / 总簇数（用 1~2 帧延迟 readback，别阻塞）
- [ ] **先补 L2 GPU 帧内计时**：NVRHI timestamp query 把真实 GPU 耗时填进 `HudFrame` 的曲线（`--profile` 按 pass 分段）。M0.5 的 HUD `frame ms` 只是 CPU 帧间隔，不能拿来验收 60fps；计划见 `docs/s0/S0-PLAN.md`「可观测性与调试工具计划」
- [ ] ImGui HUD 显示（控件加法和输入边界见 `docs/IMGUI-GUIDE.md`）；准备 1M+ 三角场景（S1 的 .nmesh 换个大模型）

**自检（S4 验收）**：相机移出视锥 HUD 绘制簇数骤降；大场景稳定 60fps。tag：`s4-done`，截图 `docs/screenshots/s4.png`（两张：面向/背对 HUD 数字对比）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M4.0 indirect 通路 | 1~2 天 | `s4-indirect` |
| M4.1 视锥剔除 | 1~2 天 | `s4-cull` |
| M4.2 统计+验收 | 1 天 | `s4-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 三级剔除链结构 | `bevy/.../meshlet/cull_instances.wesl → cull_clusters.wesl → fill_counts.wesl`（照抄结构：实例级→簇级→填计数） |
| NVRHI indirect 用法 | NVRHI / Donut 官方示例（indirect 相关 sample） |
| 概念 | kb/index.html#nanite（01 章 GPU-driven 节） |

## 底线

- 剔除 compute 必须是**无状态可重复**的（每帧从头算，不依赖上帧结果）——S5 的 DAG 遍历会替换它，接口今天留好
- readback 一律延迟帧，主循环内禁止等待型 map
