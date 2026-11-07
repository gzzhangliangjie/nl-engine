# S2 详细开发路线图：聚组 + QEM 简化 + 重切

> 对应总路线 `docs/ROADMAP.md` 的 S2。预计 4 个里程碑、业余时间 1~1.5 周。
> 这是 Nanite 离线管线**最硬的算法步**：相邻簇合并成组 → 组内整体简化到目标三角数 → 重新切簇，逐层循环。边界锁定（boundary lock）是本步的灵魂——它是「相邻 LOD 无裂缝」的唯一保证。

---

## M2.0 METIS 接入：簇聚组（1~2 天）✅ 2026-11-07（tag `s2-group`）

- [x] 集成 METIS：**vendored 源码编入**（`thirdparty/metis/`，KarypisLab 官方 METIS 5 + GKlib，自写 CMake 编静态库 `nl_metis`；改动仅 metis.h 宽度宏 + INT 宏 guard，见 VENDORED.md）
- [x] 聚组（`core/grouping.cpp`）：节点=簇、边=共享几何点（按位置 weld、忽略 UV 缝）、边权=共享点数；`METIS_PartGraphKway`（vwgt=簇三角数、固定 seed 确定性、CONTIG 仅整图连通时开）；组内连通 fixup（不连通分区按连通分量拆）
- [x] `nanite-tools simplify <model.nmesh> [-v]` 入口跑通（读 .nmesh → 聚组 → 统计打印）
- ⚠️ **口径修订（2026-11-07）**：组大小目标 = **8 簇/组**（`kTargetMeshletsPerGroup`，bevy 0.15 实证值，Nanite 用 8~32），不是旧稿的 ~50000 tri——那是 UE 的 page 口径而非 group。S3 若发现合并需求再调这一个数。
- ⚠️ 实踩（记 MILESTONES）：METIS `CONTIG=1` 要求整图连通否则 rc=-4 → 先 BFS 检测再设；断连靠 fixup 兼底。

**自检（已过）**：selftest group——单四边形 1 组、断连块无组跨块、每组在共享几何点图上连通（selftest 侧独立重算交叉验证）、空输入拒绝；Suzanne 237 簇→31 组（1 次 fixup）、球 4929 簇→617 组 avg 8.0；ctest 8/8。tag：`s2-group`。交代：展示台 s2-group 工具卡 + `docs/s2/NANITE-TOOLS-GUIDE.md` 归档。

## M2.1 QEM 简化 + 边界锁定（2~3 天，灵魂里程碑）

- [ ] QEM：组内每个顶点维护二次误差矩阵，边折叠队列按 (误差) 排序
- [ ] **边界锁定**：跨组边界的顶点与折叠边 → 误差置无穷、永不折叠（这就是防裂缝的全部机制，没有第二个魔法）
- [ ] 简化目标：组三角数减半；折叠到目标即停
- [ ] 简化后重索引（去重复顶点）

**自检**：单独测两个相邻组——简化后两组公共边界**逐顶点完全一致**（写自动对比断言，别靠肉眼）。tag：`s2-qem`。

⚠️ 常见坑：
- 边界判定用「簇归属」而不是「几何边界边」——组内两簇之间的边**不是**边界，锁了会过度锁定、简化率上不去
- QEM 退化案例（共面/细长三角）要 clamp 平面法向权重，否则塌出碎三角

## M2.2 重切 + 逐层循环（1 天）

- [ ] 简化后的组重新跑 M1.1 的切簇 → 得到 LOD1 簇集合
- [ ] 外层循环：对 LOD1 再聚组→简化→重切 → LOD2……直到组 < 最小组大小或三角数 < 阈值
- [ ] 每层写一份 .nmesh（或一个文件多 section——本步先用多文件，S3 合并时再定稿）

**自检**：每层三角数 ≈ 上一层的 50%；层与层之间簇边界对齐（可视化叠色）。tag：`s2-loop`。

## M2.3 viewer --mode lod N 验收（1 天）

- [ ] `--mode lod N` 手动渲染第 N 层；ImGui 滑条切层
- [ ] S1 验收标准在这里复检：新层簇同样 ≤128 tri、无畸形

**自检（S2 验收）**：**任意相邻层拼接无裂缝**——滑条在 N 和 N+1 之间来回切，边界处看不到洞/ crack。这是边界锁定生效的物理证据。tag：`s2-done`，截图 `docs/screenshots/s2.png`（两张相邻层对比）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M2.0 METIS 聚组 | 1~2 天 | `s2-group` |
| M2.1 QEM+边界锁 | 2~3 天 | `s2-qem` |
| M2.2 重切循环 | 1 天 | `s2-loop` |
| M2.3 无缝验收 | 1 天 | `s2-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 聚组+简化+重切全套 | `E:\UE_STUDY_2026\Carrot\engine\engine\render\ClusterManager.cpp`（最值钱的对照，读结构别抄代码） |
| METIS 用法 | Danite 仓库（Vulkan mesh shader 学生项目，离线部分短小） |
| QEM 数学 | kb/index.html#algo（04 章 QEM/边界锁定节）；Garland & Heckbert 原论文（只需 §2-3） |
| 裂缝排查 | kb/index.html#pitfalls 踩坑清单「LOD 裂缝」条 |

## 底线

- 误差单位与世界空间统一（S1 底线重申）——层间误差比较在 S3 是正确性前提
- 边界锁定的顶点集合必须持久化（S3 DAG 合并还要用），不许只活在内存里
