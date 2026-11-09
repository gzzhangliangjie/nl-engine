# S6 详细开发路线图：Hi-Z 两遍遮挡剔除

> 对应总路线 `docs/ROADMAP.md` 的 S6。预计 3 个里程碑、业余时间约 1 周。
> 视锥剔除只省「相机后面」的，Hi-Z 省「被前面挡住」的：先画一遍深度 → 手写 min 金字塔 → 第二遍逐簇盒测试剔除后再画。室内场景帧时间骤降就是验收证据。

---

## M6.0 深度 pass + 手写 min 金字塔（2 天）

> **M6.1 拍板修正（2026-11-09，三处权威口径）**：遮挡判定 = 「簇最近点 vs 区域**最远**表面」
> （UE `NaniteHZBCull.ush IsVisibleHZB`：inverted-Z 取 min = 最远表面；bevy
> `downsample_depth.wesl` reverse-Z 取 min = 最远表面 + `occlusion_cull_screen_aabb`
> 用 `aabb.max.z`=簇最近点比较）。换算到本项目径向距离度量（越近越小），金字塔必须逐级
> 取 **max**。下方 M6.0 原文与 M6.1 原文写 min 口径（「clusterMinDepth > hizMinDepth →
> 剔除」）在径向距离下**不保守会漏画**（texel 同盖近墙 3m 与远景 100m 时 min=3，簇最近
> 50m 被误剔，但簇在 100m 像素侧实际可见），已按用户拍板修正为 max；M6.0 产物
> `csHizDown`/top 回读语义随之翻转，基线随 kStage=S6.1 重拍。

- [x] 第一遍：只写深度（无颜色），输出当前 cut 全部可见簇的 depth buffer（M6.0 收口）
- [x] **compute 手写 min-mipmap**：逐级 2×2 取 min 生成金字塔——**不能用 GenerateMips**（它会做平均/过滤，不是纯 min，剔除结果错误）（M6.0 收口；M6.1 修正为取 max，理由见上）
- [x] 金字塔做成 R32F 纹理数组或单纹理带 LOD，ImGui 加层级可视化（M6.0 收口：右下角 160x160 GPU 覆盖层 + HUD mip 滑条）

**自检**：金字塔最高层 ≈ 整屏最近深度；层级可视化肉眼检查（近处亮远处暗）。tag：`s6-hiz`。
（M6.1 修正后自检口径 = top 回读三态 + hiz 自遮挡计数，见 example/s6-hiz 卡）

⚠️ 常见坑：
- GenerateMips 坑是本步第一大坑——kb/unity-handbook.html#pits 有专条，别赌「也许没关系」
- 深度是非线性（透视）的：盒测试前先在线性化空间比较（比较 z/w 或重建视图空间 z），两种空间混用 = 随机漏画（本项目全链统一视点径向距离，M6.0 已定）

## M6.1 两遍剔除：保守盒测试（1~2 天）

> 原文口径勘误见 M6.0 节顶注。2026-11-09 用户拍板两项：①金字塔 min→max；②串进结构 =
> **单 dispatch 内联**（即本节原文「一次 dispatch 完成」），bevy 式 HZB second pass
> （被遮簇压队列、本帧 HZB 复测）**不做**——那要求先做 UE-DIFFS D3a 全局队列多 pass
> 升级，触发条件（多实例/千组负载不均）未到；一帧延迟金字塔与 UE OCCLUSION_MAIN /
> bevy first pass 同口径。

- [x] 每簇投影包围球/盒 → 屏幕矩形 → 取矩形覆盖金字塔的最粗完整层级 → 查询该层级 min 深度（实现：Mara&Morgan 精确切线球投影（UE `SphereToScreenRect` 同款）+ bevy `firstLeadingBit` mip 选择 + 4×4 采样块；采样聚合取 max = 区域最远表面）
- [x] 判定：`clusterNearestRadial > hizMax`（簇最近点在区域最远表面之后）→ 剔除，整树跳过（成员球 ⊂ 父球），instanceCount 不增
- [x] 串联进 S5 的遍历 compute：视锥通过 → Hi-Z 测试 → 写 indirect（一次 dispatch 完成，还是一遍遍历）

**自检**：`--mode hiz` + 新资产 HizScene（墙 + 墙后 3 球 + 侧边对照球）：面墙看 hiz 24/85 vs dag 40/85（墙后整树剔除）；侧看 hiz == dag == 40/85（穿墙无漏画）；hiz 与 dag 画面覆盖层外逐像素 0 差。tag：`s6-cull`。

⚠️ 常见坑：
- 「最粗完整层级」取错（取了部分覆盖的层级）→ 边缘漏画；宁可取粗一级保守（bevy 对齐检查 `+3` 后再 `+1` 已内建）
- 剔除与绘制同帧：深度是**上一帧**的（一帧延迟是行业常态），快速转动时接受少量过画即可（UE OCCLUSION_MAIN / bevy first pass 同口径；本帧 HZB 复测 = D3a 升级路径）
- ⭐（M6.1 实测新增）**view space 朝向**：本项目 `camera.lookAt` 是右手系 **-z 朝前**（camera.cpp:28），Mara&Morgan/UE 公式按 +z 朝前推导——直接用 `vc.z` 会让「跨近断面」判定恒真、剔除永远静默失效（首跑 hiz==dag 40/85 的根因）；须取 `wz = -vc.z` 换轴
- ⭐（M6.1 实测新增）**帧内禁开第二个 immediate command list**（NVRHI「Two or more immediate command lists」）：懒建资源的补清屏用 just-created 标志挪到帧内 cmd 执行

## M6.2 开关对照 + 验收（1 天）

- [x] ImGui 一键开关 Hi-Z，HUD 同步显示「剔除前/后簇数」和帧时间（M6.2 收口）
  - 开关 = FrameParams.hizEnable（ImGui checkbox 双向 + `--hiz on|off` 命令行），定位同
    UE `r.Nanite.Culling.HZB`（验收用退化开关）；实现是 CB 运行时分支非 UE 的编译期
    permutation——compute 单分支开销可忽略且 ImGui 免重编 shader（差异留痕 UE-DIFFS D6）
  - 剔除前/后：dag.slang hizOccluded 命中时 u3[1] 原子累加；剔除前访问数 = emit + occluded
    （整树语义：被遮父簇的子树未访问不计入），HUD `occl : culled N / V visited (drawn X)`
  - 帧时间：GPU timer 段移出 HUD 块无条件 ensure（DeviceInfo.lastGpuMs + viewer 退出前
    `gpu timing: avg` 日志）——CPU 循环口径在 draw 轻时不敏感，GPU 口径才显性
- [x] 场景：S4 的 1M+ 大模型 + 挡板（OcclScene1M = 1.3M tri 球 + 前置红挡板盒，
  `scripts/make-occl-scene-glb.py`，挡板角宽 14.8° 盖住球投影 18.2° 的中央 ~2/3）

**自检（S6 验收，2026-11-09 实测）**：
- 被遮挡场景帧时间**明显下降** ✓：1920x1080 unthrottled NVRHI timer 3 轮均值
  on ~2.74ms vs off ~4.03ms（**-32%**）；960x540 下 GPU 口径差距小（prepass 重复开销
  ≈ 绘制节省，分辨率升到 1080p 主 pass 像素着色占比提升后才显性——验收用 1080p）
- 关掉开关立即回退 ✓：`--hiz off` → visible 40/85 == dag 基线、occluded 0（HizScene）；
  OcclScene1M off → 12324/43547 vs on 6861/43547（84% 簇被剔，occluded 5378 of 12239）
- 遮挡边缘无漏画 ✓：OcclScene1M hiz on/off 最终画面 **MD5 逐字节一致**（被剔簇本就被
  深度测试挡住）+ 侧视 yaw 75 occluded 0 零误剔
- HUD 布局坑（M6.2 实测）：960x540 画布面板高被 min(cap, H-28)=512 钉死，hiz 区放原位
  （frustum/cull GPU 行后）会掉进滚动区截断——整段上移到 mode/model 行后，仅 hiz 模式
  布局变化（其它模式一像素不动）

tag：`s6-done`，截图 `docs/screenshots/s6.png`（开/关 HUD 对比，两张 960x540 拼接）✓

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M6.0 深度+金字塔 | 2 天 | `s6-hiz` |
| M6.1 两遍剔除 | 1~2 天 | `s6-cull` |
| M6.2 开关验收 | 1 天 | `s6-done` |
## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 金字塔/盒测试细节 | kb/index.html#algo（04 章 Hi-Z 节） |
| GenerateMips 坑 | kb/unity-handbook.html#pits（同源问题） |
| Nanite 官方两遍结构 | Karis《Nanite: A Deep Dive》；UE `NaniteCullRaster.cpp`（RasterHomogeneous passes） |

## 底线

- Hi-Z 必须是**可整体开关的独立层**（验收条件之一），和 S5 遍历之间只通过「是否执行第二段判定」耦合
- 深度空间一致性（线性化）从 M6.0 起贯穿——S9/S10 的深度重建全部沿用同一套约定
