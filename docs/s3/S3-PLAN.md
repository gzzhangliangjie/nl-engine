# S3 详细开发路线图：构建 Cluster DAG + Fallback Mesh

> 对应总路线 `docs/ROADMAP.md` 的 S3。预计 3 个里程碑、业余时间约 1 周。
> 把 S2 产出的各 LOD 层连成有向无环图（DAG）：父节点误差 = max(子)，运行时 S5 靠「屏幕误差 ≤ 阈值即 cut」选层。fallback mesh 是给 S8 光追用的低模，顺手产出。

---

## M3.0 DAG 构建自底向上（2~3 天）

- [ ] 逐层向上：LOD n 的簇聚组（复用 M2.0）→ 组 = DAG 父节点，组内子簇 = 子节点边
- [ ] 父误差：`parent.error = max(child.error)`（Karis 论文规则）；组简化重切产物即父簇
- [ ] 注意：父簇的顶点引用子簇顶点（共享边界），**DAG 边表达「可被父替换」**，数据上父存子簇 id 列表
- [ ] DAG 写回 .nmesh v2（加 section：dagGroups[]，每组存 childIds + error + bounds）

**自检**：随机抽 1000 个父节点断言 `error == max(children.error)`；图无环（DFS 染色检测）。tag：`s3-dag`。

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
