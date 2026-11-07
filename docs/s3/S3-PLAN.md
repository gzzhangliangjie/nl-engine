# S3 详细开发路线图：构建 Cluster DAG + Fallback Mesh

> 对应总路线 `docs/ROADMAP.md` 的 S3。预计 3 个里程碑、业余时间约 1 周。
> 把 S2 产出的各 LOD 层连成有向无环图（DAG）：父节点误差 = max(子)，运行时 S5 靠「屏幕误差 ≤ 阈值即 cut」选层。fallback mesh 是给 S8 光追用的低模，顺手产出。

---

## M3.0 DAG 构建自底向上（2~3 天）✅ 2026-11-07

- [x] 逐层向上：LOD n 的簇聚组（复用 M2.0）→ 组 = DAG 父节点，组内子簇 = 子节点边
- [x] 父误差：组 error = max(组简化误差， 全部成员簇误差)（UE ClusterDAG.cpp:1441/:1587，比 Karis max(children) 更严：含本次简化引入的误差）；父簇 bounds.error 统一携带组误差（UE:1593），沿链单调
- [x] 实做修正（三家源码实读定稿）：**按组重切，父簇绝不跨组**（UE ReduceGroup / bevy :154）——M2.2 的全局重切会让父簇跨组 → 每父簇生成组不唯一 → S5 组原子 cut 不成立；DAG 边用双向记法：组存成员簇 id（clusterGroup[]，子方向）+ genFirst/genCount 区间（父方向，连续铺满下一层），不存独立 childIds 列表
- [x] DAG 写进 .nmesh meta version 2（尾部 section：u32 groupCount + 32B 组记录×N + u32 clusterGroup×meshlets；v1/旧 v2 逐字节兼容读回）

**自检**：selftest dag 不抽样——128x128 合成网格全链逐层逐组全量断言 error == max(simplifyError, members)（逐位精确）、组图 DFS 染色无环、gen 区间铺满、v3 roundtrip、篡改必被抓住。tag：`s3-dag`。

⚠️ 常见坑：
- 不同分支的组可能共享子簇（这就是 DAG 不是树）——构建时别按「层」拍平存储，按组引用存
- 组内简化**跨组边界锁定依旧生效**，否则 DAG 上层直接裂缝（S2 的坑在这层会二次出现）

## M3.1 Fallback mesh 导出（0.5 天）

- [ ] 取每 mesh「误差可接受的最顶层」整体连成普通 indexed mesh（或取根层），作为 `.nmesh` 内的 fallback section
- [ ] 用途说明写进文件头注释：S8 BLAS 用它，不追 DAG

**自检**：fallback 独立渲染正常无洞。tag：`s3-fallback`。

## M3.2 viewer --mode dag-top 验收（1 天）

- [ ] 只渲染 DAG 最顶层（根组簇）——画面应是一个正确但粗的模型
- [ ] 临时调试：任选一个组「下钻一层」渲染，验证父子衔接无洞

**自检（S3 验收）**：顶层渲染 = 一个正确粗模；遍历能从根走到任意叶且中途每层画面完整无洞。tag：`s3-done`，截图 `docs/screenshots/s3.png`（顶层 + 中间层 + 叶层三连）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M3.0 DAG 构建 | 2~3 天 | `s3-dag` |
| M3.1 fallback | 0.5 天 | `s3-fallback` |
| M3.2 可视化验收 | 1 天 | `s3-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| DAG 概念与合并细节 | kb/index.html#nanite（01 章 DAG 节）；Carrot《Recreating Nanite: [The Plan](https://blog.jglrxavpok.eu/2023/11/12/recreating-nanite-the-plan.html)》；逐篇实录 `docs/blogs/jglrxavpok-carrot-nanite.md` |
| 完整离线流程对照 | `bevy/.../meshlet/from_mesh.rs`；JMS55 博客 [0.14 篇](https://jms55.github.io/posts/2024-06-09-virtual-geometry-bevy-0-14/)；实录 `docs/blogs/jms55-bevy-meshlet.md` |
| DAG 质量秘方 | JMS55 [0.16 篇](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16/)——METIS 直接聚三角形（共享顶点最少化，UFactor=1、划分数少算）；bevy 一直保留 DAG，BVH 只是剔除遍历层（main 的 `cull_bvh.wesl`），两者配合不是替代 |
| .nmesh 结构演进 | 自己的 `docs/s1/S1-PLAN.md` 底线（版本号纪律） |

## 底线

- DAG 是「图」不是「树」：共享子簇只存一份，父节点存引用——数据结构错了 S5 遍历会重复画
- fallback mesh 必须在这步定稿，S8 直接读，不许到 S8 再「顺手生成一份」
