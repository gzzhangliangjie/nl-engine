# Carrot 开发过程导读：用 1180 个提交当「实况教材」

> Carrot 仓库带完整历史（~1180 提交，2020 → 今）。博客是事后总结，**git 历史是实况录播**——能看到 jglrxavpok 每一步怎么踩坑、返工、优化。本导读把 Nanite×GI 相关的提交时间线抽出来，对齐到我们的 S1~S13。
> 配合博客系列食用：The Plan (2023-11) → Material Pass (2024-02) → Raytracing (2024-08) → GI 长文。

## 怎么挖（三条命令就够）

```bash
git log --all --oneline -i --grep="virtual geometry"     # 按关键词看时间线
git show 963eba7d                                        # 看某次提交的完整 diff（重点功能）
git log --oneline --follow -- engine/engine/render/ClusterManager.cpp   # 单文件演进史
```

进阶：`git log -S "meshlet" --oneline`（pickaxe，找「这个概念第一次出现」的提交）。

## 时间线 → 我们的步骤（实测抽取）

| Carrot 实况（hash 可直接 show） | 时间 | 对应我们 | 能学到什么 |
|---|---|---|---|
| `701b7f95` 编辑器加开关启用 virtual geometry | 2023-12 | S1~S3 | **先做成可开关的平行模式**，与旧渲染共存——和我们 `--mode` 设计同构 |
| `574b7cae` 接入 meshoptimizer clusterlod（「先修 crash 再用」） | ~2024 | S1 | 作者也用 meshoptimizer 起步；注意他先集成再排坑的节奏 |
| `1f1eee26` `a3b21c0f` 视锥剔除 + 调 workgroup size | 2024-04-20 | S4 | 剔除是后补的——先画出来再剔除，别一开始就上 GPU-driven 全家桶 |
| `83bb40ee` `c362e271` 材质 pass 插值连修两次 | 2024-04-22~24 | S7 | **vis buffer 反解的插值 bug 是必踩坑**（perspective-correct），他有心理准备：博客里也写了 |
| `84bb2f70` packed vertex + **readback buffer 只存已画簇 id** | 2024-05-23 | S13 | 回读方案和他一致；packed vertex 是为了省显存（同一动机：RT 要吃几何） |
| `963eba7d` **BLAS 按组建而不是按簇建**（「makes it viable in terms of memory」） | 2024-05-27 | S13 | ⭐ 关键决策：逐簇 BLAS 显存不可行，按聚组建才是工程答案——比我们文档写的还细一层 |
| `da5b1d35` timeline semaphore 做异步 AS 拷贝 | 2024-06-04 | S13 | 聚合构建的异步化；NVRHI 下对应 fence/queue 纪律 |
| `0831b814` AS 内存预分配 + 多线程录命令 + 回读瘦身 | ~2024 | S13 | 性能三连：预分配/并行/减回读数据量 |
| `8dff3ee1` 「Sample from diffuse lobe to make GI work again」 | 早于 GI 文 | S10 | GI 采样分布错=全黑，作者也在这里挣扎过 |
| `ce17c904` RT buffer 生命周期 crash 修复 | 后期 | S13 | AS 引用的底层资源销毁顺序——经典 crash |

## 三个「实况才看得到」的开发教训

1. **顺序和我们路线图相反**：Carrot 是先有引擎/编辑器，2023 年底才往里塞虚拟几何；我们是先啃渲染再长引擎（P8）。好处：你不会被引擎壳分心；代价：没有编辑器调参界面，全靠 ImGui + `--mode`（所以 S0 把模式分发器一次定型很重要）。
2. **材质 pass 他修了三天**（两次 interpolation 提交）——S7 的「逐像素一致对照」验收就是防这个，别跳过对照直接上多材质。
3. **RT 集成花了 2 个月**（2024-05 → 2024-08 博客才发），其中大头是显存和异步，不是算法——S13 文档里「预建+池化+绝不同帧回读」三条底线全是从这段历史里蒸馏的。

## 用法约定

- 卡住某步时：先读对应行的 `git show <hash>`，看作者当时的 diff 规模（通常单功能 200~600 行）——**你的实现粒度应该和它同量级**，超了就是在过度设计
- 想看「完整功能从无到有」：从时间线第一个 hash 开始顺着 `git log --reverse` 往下翻到下一个里程碑，比读最终代码高效
