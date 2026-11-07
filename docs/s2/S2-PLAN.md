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

## M2.1 QEM 简化 + 边界锁定（2~3 天，灵魂里程碑）✅ 2026-11-07（tag `s2-qem`）

- [x] QEM：组内每个顶点维护二次误差矩阵，边折叠队列按 (误差) 排序（core/simplify，面积加权平面二次型 + 有序集队列，平局按点编号决胜 → 完全确定）
- [x] **边界锁定**：跨组边界的顶点与折叠边 → 误差置无穷、永不折叠（这就是防裂缝的全部机制，没有第二个魔法）。实现细化：锁定点判定看组归属（被 ≥2 个组的簇引用）；双锁定端点的边（接缝本体）不折叠；被摧毁三角形不得是某条双锁定边（≠本边）的唯一三角形——否则接缝边在本组一侧没三角形了照样裂
- [x] 简化目标：组三角数减半；折叠到目标即停（`simplifyGroups(mesh, grouping, out, error, ratio=0.5)`）
- [x] 简化后重索引（去重复顶点）（canonical weld 天然去重；输出 SimplifyResult.indices/pointPositions/lockedPoints）

**自检（已过）**：selftest qem——64x64 网格 22 组、807 边界点：锁定点集合与独立重算边界集合一致、简化后边界**逐顶点位级不变**（自动对比断言，不靠肉眼）、接缝边两侧组都有三角形、同输入重跑逐字节一致。Suzanne 3936→1976（50.2%）；oracle meshopt 49.5% 同档。tag：`s2-qem`。

⚠️ 实踩（记 MILESTONES）：三角局部索引要先加 `vertexOffset` 再查顶点（canon[m.vertexOffset + triangles[base+k]]，两层包错会段错误/编译错）；覆盖哨兵别用 0（合法顶点号，用指针判空）；球模型 54.4% 是 2-tri 折叠越过目标/被 link condition 拒所致，口径内。

⚠️ 常见坑：
- 边界判定用「簇归属」而不是「几何边界边」——组内两簇之间的边**不是**边界，锁了会过度锁定、简化率上不去
- QEM 退化案例（共面/细长三角）要 clamp 平面法向权重，否则塌出碎三角

## M2.2 重切 + 逐层循环（1 天）✅ 2026-11-07（tag `s2-loop`）

- [x] 简化后的组重新跑 M1.1 的切簇 → 得到 LOD1 簇集合（`core/lod` 复用 `buildMeshlets`，法线按邻面重算、UV 置零）
- [x] 外层循环：L1 **重新聚组**→按当前分组重算边界锁→每组硬目标 50% 简化→重切 → L2……直到三角数 < 阈值 / 组数不足 / 三角数不再下降 / 达到 maxLevels
- [x] 每层写一份 `.nmesh`（多文件 `<prefix>.lodN.nmesh`；S3 合并时再定稿）；**`.nmesh` v2** 尾部 section 持久化 `level / maxError(parent error) / lockedPositions`，v1 逐字节兼容
- [x] **边界锁每轮重算、不跨层继承**：严格对齐 bevy `from_mesh.rs:114-120/520-523`（`lock_group_borders` 每轮全量覆写 `vertex_locks`）、UE `Cluster.cpp:1023-1039`（`LockPosition` 只作用当前 group）、meshoptimizer `simplifier.cpp:526-529`（`SimplifyLockBorder` → `Kind_Locked`）。跨层无裂缝由 S3 DAG cut 以 group 为原子单位保证

**自检（已过）**：selftest loop 全绿——灵魂断言「每层锁点集合 == 独立重算的当前分组边界集合」（证明不继承）、每层约减半、parent error 单调、簇上限、v1/v2 读回、确定性重跑、负向拒绝；Suzanne **6 层** 3936→1976→985→490→246→124 tri，锁点 622→117→84→35→0→0 递减（每层重算）；64x64 网格 7 层；ctest 12/12、回归总闸 ALL GREEN。证据图 `example/s2-loop/lod-suzanne.png`（六栏同机位同尺度 + 红点边界点）。tag：`s2-loop`。

⚠️ 实踩：第一版自造了「跨层累积锁定 + `minReduction 30%` 提前收敛」，锁集合单调增长导致 L3 只能降 9%、只出 3 层；核三份源码后**整套推翻重做**为标准循环，删掉累积与门槛，同一资产自然收敛出 6 层、每层严格约 50%。M2.1 的边界锁实现不变（本来就是标准 `LockBorder`）。`groupClusters` 在簇数 ≤8 时 nparts=1，METIS_PartGraphKway 不接受 nparts<2 → 走单组直通 + 连通性 fixup。

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
