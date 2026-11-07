# S1 详细开发路线图：切簇（meshlet 构建）

> 对应总路线 `docs/ROADMAP.md` 的 S1。预计 4 个里程碑、业余时间约 1 周。
> 阶段 P1 的第一步：把普通 mesh 变成「簇的集合」，这是后面一切 Nanite 特性的原料。
> **跑不通当下一步，不攒债。**

---

## M1.0 nanite-tools 骨架 + glTF 读取（0.5~1 天）✅ 2026-11-07（tag `s1-tools`）

- [x] `nanite-tools/` 目录建 CMake target（S0 预留过），命令行框架：`nanite-tools <subcommand> <args>`（手写或 cxxopts，别纠结）
- [x] 子命令占位：`cluster / simplify / dag / stat`（本里程碑只实现 cluster 的入口）——占位子命令调用必须**显式退出 2**（防静默假成功）
- [x] glTF 读取：**复用 M0.4 已经在用的 tinygltf**（`thirdparty/tinygltf/`，从本机 Carrot vendored；M0.4 从未引入 fastgltf），把 `renderer/src/scene.cpp` 的解析部分抽成 `core/mesh_io`，让 viewer 与 nanite-tools 共用一份代码。注意：tinygltf 实现宏只在**一个 TU** 展开（现在在 `scene.cpp`，抽走时要同步移走），且 `core/` 仍保持零图形 API 符号
- [x] 中间数据结构：`Vertex{pos,normal,uv}` + 三角列表 + mesh 级变换展开（glTF 节点变换烘进顶点）

**自检**：`nanite-tools stat model.gltf` 能打印「顶点数 / 三角数 / 子网格数」。tag：`s1-tools`。
**交代（2026-11-07 固化）**：展示台 s1-tools 版本卡 `kind:"tool"` 用例现场实跑（usage / stat×三资产下拉 / 未实现负向）+ `docs/s1/NANITE-TOOLS-GUIDE.md` 归档；规范在 `docs/ROADMAP.md`「里程碑交代规范」节。

## M1.1 meshlet 构建（2~3 天，本步的核心算法）

- [ ] 贪心构建：从任意三角出发，逐个吸收相邻三角，直到满 128 tri / 64 vert 上限（vertex 共享优先，空间相邻次之）
- [ ] 每簇算：**包围球**（Ritter 或 EPOS6，先 Ritter）、**圆锥包围**（法线锥，后面剔除用）、**初始误差**（边界边在世界空间的投影长度）
- [ ] 数据结构即最终 `.nmesh` 的簇结构，一次定好带版本号：

```
struct Meshlet { uint32 triOffset, triCount, vertOffset, vertCount; Sphere bounds; Cone cone; float error; }
struct NMeshHeader { uint32 magic, version; uint32 meshletCount, ...; }   // 后面 S2/S3 只加字段不改旧字段
```

**自检**：单元测试——随机网格切完后每簇 tri ≤ 128、vert ≤ 64；顶点覆盖率 = 100%（所有三角都被分进簇）。tag：`s1-cluster`。

⚠️ 常见坑：
- 贪心吸收时**忘了 vert 上限**（64），只有 tri 上限——跑大模型时簇爆炸，后面 indirect buffer 溢出
- 误差单位要一开始就统一成「世界空间长度」，别用局部空间——S3 DAG 的父误差传播全部依赖同一单位

## M1.2 .nmesh 资产读写 + stat（0.5~1 天）

- [ ] 写盘：`nanite-tools cluster model.gltf -o model.nmesh`（二进制，header + meshlet 数组 + 顶点/索引）
- [ ] `stat` 子命令完善：簇数、平均/最大簇大小、误差分布直方图（ASCII 打印即可）

**自检**：写出的 .nmesh 能被读回且逐字节一致（roundtrip 测试）。tag：`s1-asset`。

## M1.3 viewer --mode cluster 簇着色可视化（1 天）

- [ ] viewer 加载 .nmesh，`--mode cluster` 下每簇随机颜色（hash(clusterId) → 调色板）
- [ ] ImGui 加一个簇 id hover 显示信息（可后补）

**自检（S1 验收）**：调试图簇边界清晰、颜色块大小均匀、**无长条畸形簇**（畸形=纯按索引顺序切的标志，说明吸收逻辑没优先空间相邻）。tag：`s1-done`，截图存 `docs/screenshots/s1.png`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M1.0 工具骨架+读取 | 0.5~1 天 | `s1-tools` |
| M1.1 meshlet 构建 | 2~3 天 | `s1-cluster` |
| M1.2 .nmesh+stat | 1 天 | `s1-asset` |
| M1.3 可视化验收 | 1 天 | `s1-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| meshlet 构建参考实现 | `E:\UE_STUDY_2026\bevy\crates\bevy_pbr\src\meshlet\from_mesh.rs`（离线构建入口，Rust 但逻辑直白）；meshoptimizer `src/meshlet.cpp` |
| 资产结构设计 | `bevy/.../meshlet/asset.rs` |
| 概念不清 | kb/index.html#nanite（01 章 meshlet/包围球/误差节） |
| 簇质量差/畸形 | JMS55 博客 [0.14 篇](https://jms55.github.io/posts/2024-06-09-virtual-geometry-bevy-0-14/)「what didn't work」一节；逐篇实录 `docs/blogs/jms55-bevy-meshlet.md` §1.3 |

## 底线

- 簇上限常量（128/64）必须集中定义在头文件，S2 重切和 S4 剔除都引用它
- .nmesh 从第一天带版本号——S2/S3 会加字段，不许破坏旧文件
