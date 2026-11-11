# S11 详细开发路线图：世界缓存（Surface Cache / Hash Grid）

> 对应总路线 `docs/ROADMAP.md` 的 S11。预计 4 个里程碑、业余时间约 2 周。**全路线最难的单步，多跳 GI 从这里涌现。**
> GPU hashmap 世界网格：每 cell 存 irradiance，每帧对光源追 1 条射线（更新自身）+ 1 条随机方向射线（查邻居，形成二跳反馈）；探针射线命中后不再「用直光近似」，改为查缓存——多跳就是查到的缓存里已经带着上一帧的反弹。

---

## M11.0 GPU hashmap 基础设施（2~3 天）

- [ ] 世界空间均匀网格：cell 大小常量（按场景尺度，如 0.5m）；hash(x,y,z) → 线性探测桶数组
- [ ] 插入/查找/删除 + **life 衰减回收**（无墓碑，Q11-0 已拍板）；帧计数做 LRU 候选
  - 📋 预研证据（`docs/s10/GI-RESEARCH-NOTES.md` §2，2026-11-09）：**bevy 与 Carrot 都不用墓碑**——bevy 清空 checksum + 线性探测上限 3 步，Carrot 用自含式 16 槽桶；断链靠「cell 按内容 key、下次查询自然重插」自愈 + 时域混合掩盖。
  - ✅ **Q11-0 拍板（2026-11-11）**：回收策略 = 方案 A，跟 bevy/Carrot——无墓碑，life 衰减到 0 清零**立即重用** + 线性探测上限 3 步 + 查询命中续命；自愈靠「cell 按内容 key、下次查询自然重插」，闪烁靠时域混合压住。M11.3 稳定性验收若实测闪烁超标，再议补「延迟一帧复用」小补丁（现不做）。
- [ ] `--mode cache-viz`：命中 cell 伪彩色显示占用率

**自检**：插入/查找/删除单元测试（CPU 侧仿真先行）；viz 图 cell 分布符合场景。tag：`s11-hash`。

⚠️ 常见坑（2026-11-09 三处精读后修订，详 `docs/s10/GI-RESEARCH-NOTES.md` §2）：
- **cell 回收重用最经典的 bug**：回收的 cell 立刻被新数据占用，旧桶探测链断掉 → 邻居查询失败闪烁。经典解是墓碑 + 延迟回收；但两处参考（bevy/Carrot）都直接清零立即重用，靠短探测链 + 自愈 + 时域混合压住闪烁——**Q11-0 已拍板方案 A（2026-11-11）：无墓碑 + 立即重用，见 M11.0 第一节**
- **radiance 原子累加必须 int 量化 + 双缓冲帧末合并**（Carrot：int3 × 1/16384 量子 + InterlockedAdd 进独立 accumulation buffer，mergeRadiance 帧末 lerp cap 64）——直接在 radiance 上原子加 float 不可行
- **key 与 checksum 必须两个独立哈希**（Carrot pcg+xxhash / bevy pcg+iqint），同哈希自校验会假命中
- **每帧更新预算用软目标控**（bevy：每 cell 以 target/count 概率决定本轮是否追射线）——全量每帧追会把射线预算打爆
- **cell 尺寸 LOD 第一版不做**：Carrot 写了 LOD 公式但实测强制关掉（固定 0.15m），bevy 用 fract³ 概率进位软化跳变；LOD 闪烁是实况教训，列 S14 升级

## M11.1 cell 更新：光源射线 + 邻居射线（2 天）

- [x] 每帧对每个活跃 cell：1 条**对光源**的 shadow ray（判可见性）+ 1 条**随机方向**射线（命中别的 cell → 把它的 irradiance 乘 albedo 加进自己——多跳反馈的机制本体）
- [x] 随机方向每帧变化（帧号入 seed），多帧覆盖球面

**自检**：`cache-viz` 中 cell 颜色随帧号逐渐丰富（单帧噪声多帧收敛）。tag：`s11-update`。

- ✅ **Q11-1 拍板（2026-11-11）**：cache-viz radiance 模式的 example 卡断言形态 = **方案 A 统计断言**。根因实锤：每帧 rebuild 的 AS build 内部布局 D3D12/Vulkan 规范不保证 bit-exact，GI 邻居查询把 hitP 量化进 cell key，量化边界翻转 → mode 1 radiance 图**天生非逐字节可复现**（实测 3 次运行 3 个 md5、约 9% 像素差 maxDelta 268）。落地：GPU 侧改统计口径（`tools/check-s11-update.py`：伪彩覆盖 ≥0.10 + 直光/背光带 luma margin ≥5.0，标定实测余量 3 倍以上）；CPU 侧 `cachesim --update` 保持 bytes 级确定断言不变。
- ✅ **实施实录（2026-11-11）**：cell 从命中点缓存升级为**存入射辐照度 C**（E_direct = lightCol·I·NdotL·shadow；E_ind = albedo×C(hit)，lambert f×cos÷pdf 约分；miss 天空 albedo×skyGradient——bevy 的 π×env 是其 cell 存出射口径所致，我们不抄）；软目标 p=0.25 为 bevy target/count 的无 active-list 近似；blend 只处理 newMark cell、1/min(n+1,64) 线性 cap、**消费后清标记**。新增 selftest 8~10 组共 22 断言（DI 增量收敛差分法 / 二级弹射涌现 / blend 语义）。踩坑：blend 首版漏清标记 → newRadiance.w 恒 1 把第二帧起 update 全挡（帧首 w>0 防御与标记生命周期冲突，防御已删）；blend PSO 创建失败 = slang 消除未用绑定后反射缺 t5 → layout 补 SB_SRV(5)。`direct.slang` 0xdeadbeef 守卫注释如实化：**s10-refgi 冻结基线的 GI 项实为纯天穹**（该段从不执行），留作后续重开的现成实现；**Q11-2b 已拍板本轮 M11.2 不重开**（名义不符列 M11.3 候选），重开仍须单独拍板 + 重拍 4 张基线，禁顺手改形态。

## M11.2 探针查缓存 + 二跳 gather（1~2 天）

- [x] S10 探针射线命中点改查世界缓存（不再用直光近似）；未命中 cell 才退回直光近似
- [x] final gather 不变——多跳已经通过缓存反馈自然出现

**自检**：红白棋盘**颜色渗透出现**；关灯后余晖**自然衰减**（几帧内渐暗不是瞬间黑）。tag：`s11-bounce`。

- ✅ **开工拍板四项（2026-11-11，用户单轮四问 + 追问定案）**：
  - **Q11-2a = 方案 A「新开关 + 新卡」**：现状 `drawCacheFrame` 仅 `params.cacheViz` 触发（scene.cpp 实读），probe 模式下缓存表恒空。本轮新增 `--probe-cache-bounce`（**默认关**），触发扩为 `cacheViz || bounce`；**帧序保持 trace→cache→gather 不动**（trace 读上一帧收敛值，bevy 同款时域滞后；前移会破坏 csCacheFill 依赖本帧 probeDepth_ 的契约）。老 s10-* 12 张冻结基线**零漂**（M9.1 语义变更先例：老卡锁定、新语义自带新卡），M11.2 自带 `s11-bounce` 新基线卡。
  - **Q11-2b = 方案 A「本轮不重开」**：`direct.slang` 0xdeadbeef 守卫段**原样不动**（段内容是 M10.1 单跳直光近似，不是查缓存二跳实现；且与 Q11-2a 的老卡零漂直接冲突）。`reference-gi` 继续当 Q14-1 单跳真值/趋势尺。⚠️ **名义债登记（用户「记入S11」）**：s10-refgi 冻结基线 GI 项实为纯天穹，与 Q14-1 拍板的「单弹射 GI」名义不符（M11.1 实锤的历史形态债，属 M10.3 的账）——列 **M11.3 候选**，重开须单独拍板 + 重拍 4 张基线，禁顺手改形态。
  - **Q11-2c = 方案 A「双 albedo 口径」**：命中写 `albedo_hit × C(hit)/π`（与 M11.1 的 E_ind=albedo×C(hit)、viz radiance 模式同口径，延续 Q14-1 双 albedo 物理）；**miss 才退回原直光近似**（含 ambient 微底）；查询复用 M11.1：`hn=-d` 近似法线 + bias 抬升量化 + `wcMakeKey` 3 步探测，读 `wcRadiance_`。
  - **✅ Q11-2c 补正拍板（2026-11-11，CPU 仿真实测反例后用户拍板 A）**：命中分支改为 **`albedo_hit × (C(hit)/π + ambient)`**——命中也带 Q13-2 微底，与 miss 兜底分支同起跑线。反例实锤：原拍板 `albedo_hit×C/π` 不含 ambient，而 ambient 0.03 只在兜底分支，开阔天空场景下背光区**新路径 0.03292 反比老路径 0.03420 暗**（`gi-tools bounce` 实测），GPU 开关开/关自比的渗透增量会是负的。补正后两分支同底，渗透增量正向可检出。CPU/GPU 同步改一行，Q11-2c 的「双 albedo / miss 退回原近似」主体不变。
  - **Q11-2d = 方案 A「CPU+GPU 统计」**：仓库无红白棋盘场景资产、CLI 无关灯开关（traceCB 灯强度硬编码 2.5）。本轮 **CPU 先行**：`gi-tools --bounce` 加渗透 + 关灯余晖时间序列（bytes 级确定断言）；**GPU**：现有 Suzanne 走「同管线开关开/关自比」统计增量（Q11-1 判据：不依赖 AS 数值才许 bytes，依赖则统计）+ 新增 `--probe-light-scale` 做关灯亮度衰减序列。**不造新场景资产**；`probe_gather.slang` 与 gather 段按 PLAN 不动。
  - **⚠️ Q11-2d GPU 验收口径勘误（2026-11-12 实测）**：原「合成图 `--mode probe` 开关开/关自比」在 **Suzanne 凸体 + 纯天空场景结构性读不到 M11.2 增量**——`csGather` 每像素只采样 atlas 的**外法线方向** texel（`octaDirToTexel(normalize(n), octRes)`），凸体表面外法线恒指向相机外→恒 miss→天空 texel；M11.2 真正改写的 `g_irrad` **命中方向 texel** 永不被 gather 读到 → 合成图天生 **ON≡OFF（byte-identical）**，是盲通道非缓存 bug。正确验收 = 新增 `--probe-atlas-dump`（把 `irradAtlas_[0]` 直出画面、绕开 gather）+ `tools/check-s11-bounce-atlas.py onoff` 比 ON/OFF（断言：非逐字节相同 + 均值差≥0.2 + 变化像素≥0.3% 且成片落在物体区）。`example/s11-bounce/run.bat verify` 双后端全绿即此口径。
- **落码面（摸底实读）**：`probe_trace.slang` 命中分支（traceCB 扩槽 / traceSet 增 `wcChecksums_`+`wcRadiance_` 两 SRV，Q4 冻结面只加 SRV 不动 UAV）、`scene.cpp` drawCacheFrame 触发条件 + 帧首状态登记、`renderer.hpp` FrameParams 两字段、`viewer/main.cpp` 两 CLI 开关；查询逻辑复用 world_cache.slang `wcMakeKey` + 3 步探测。
- **冻结面清单**：Q4（不新增独立 GBuffer 纹理 / MRT / 落盘 UAV）、s10-* 12 张冻结基线 PNG、`kStage` 仍 S8.2（不 bump → --hud 用例不重拍）、0xdeadbeef 守卫形态、Q11-1 统计断言判据。

- **✅ M11.2 收官记录（2026-11-12，tag `s11-bounce`）**：
  - **落码**：`probe_trace.slang` 命中分支删旧 TEMP DEBUG 品红块、`hn=dir`（与 csCacheFill 朝眼定向同向量，轴对齐面量化后一致）+ `ptMakeKey(hq, hn)` 查 `wcChecksums_`/`wcRadiance_`；新增验收工具 `probe_atlas_dump.slang`（`csIrradDump` 把 `irradAtlas_[0]` 直出画面）+ `scene.cpp` `drawIrradDump` + `renderer.hpp` `probeAtlasDump` + `viewer/main.cpp` `--probe-atlas-dump` + `CMakeLists.txt` `_ENTRIES_PROBE_ATLAS_DUMP`。`probeCacheDiag` 调试伪彩 flag 保留（无害，未删）。
  - **踩坑（写实）**：OFF 态 atlas-dump 裸跑 252 个 D3D12 device error（`ProbeIrradAtlas`/`ProbePrevDelta` 帧末 UAV vs SRV 冲突）——根因 OFF 不跑 `drawCacheFrame`、跳过帧末 `irradAtlas_`/`probePrevDelta_`/`probeAtlas_` 的 UAV→SRV 跨帧收尾（drawCacheFrame L4131-4142），下一帧 `beginTracking` 声明 SRV 与实际 UAV 态冲突；修 = `drawIrradDump` 末尾补同样的跨帧收尾块。双后端 ON/OFF 均 `deviceErrors=0` + `exit 0`。
  - **验收铁证（双后端全绿，`example/s11-bounce/run.bat verify`）**：CPU `gi-tools selftest`（ctest `s11-gi-selftest`）双后端 `bounce: early 0.31920 late 0.35212 ctrl 0.31920 hits 96/96`（hits 从旧 hn=-d bug 的 0 → 96，late>ctrl 证明缓存收敛亮度上升）；GPU atlas-dump ON vs OFF：DX12 mean 0.456 / 变化像素 1.26% / 质心 (253,479)，VULKAN mean 0.502 / 变化像素 1.75% / 质心 (248,470)，均非逐字节相同 + 成片落在物体区。合成图 `--mode probe` 在本场景 ON≡OFF（盲通道，见上条 Q11-2d 勘误），故 GPU 验收以 atlas-dump 为准。
  - **下一步**：M11.3 稳定性验收（cell 生命周期 / 快速移动 / 名义债 Q11-2b）；S12 历史帧并轨。

## M11.3 稳定性验收（1~2 天）

- [x] cell 生命周期打磨：回收延迟、新 cell 置信度预热（避免新 cell 一帧全亮闪）→ **M11.3 收官实测（2026-11-12）：不补**。方案 A（无墓碑+立即重用+时域混合）稳态收敛实证：静态 6 档长帧数（64~512）末帧 atlas 相邻差 <0.026、static 64 帧序列逐帧差全 0（bytes 级）——无残影死 cell、无回收闪烁，无预热必要（Q11-0 拍板「超标才补」，实测无超标）。
- [x] 摄像机快速移动压力测试 → **沙箱工具化完成（替代裸机录屏）**：`viewer --orbit-speed <deg/帧>` + `--screenshot-seq <dir>`（确定性旋转 + 逐帧回读）。移动 1.5°/帧×64 帧：ON（mean 1.23/max 6.60）≤ OFF（mean 1.32/max 6.66），最大 spike ON/OFF 几乎相同（视角公共项），VULKAN 1.2340 与 DX12 一致，全程 deviceErrors=0 → 无 cell 涌现感。录屏 `docs/screenshots/s11_*.png` 由本口径替代。
- [x] **Q11-2b 名义债（用户拍板 2026-11-12「现在重开」）**：s10-refgi 冻结基线 GI 项实为纯天穹（0xdeadbeef 守卫恒 false），与 Q14-1 拍板的「单弹射 GI」名义不符；恢复名义 = 重开该段 + 重拍 refgi/compare 4 张基线 + 重标定 trend 门，须单独拍板，禁顺手改形态 → **已重开**：`direct.slang` 守卫改 `if (true)`（段内逻辑未动），4 张基线重拍（旧图备份 `backup-pre-q11-2b/`，新旧 meandiff ~0.2/255 = 弹射项真实生效、方向变暗物理合理），trend 0.959/3.5（门限 0.60/30 不变），run.bat verify 全绿；老路径 s9-reference/s10-probe 各 2/2 bytes 保形。

**自检（S11 验收）**：渗透+余晖稳定出现（M11.2 CPU+GPU 双链实证）；cell 回收**无闪烁**（M11.3 稳态收敛实证）；快速移动无明显 cell 涌现感（M11.3 ON≤OFF 逐帧差分实证）。tag：`s11-done`，录屏 `docs/screenshots/s11_*.png`（→ 由 `--orbit-speed`+`--screenshot-seq` 序列口径替代）。

**S11 收官记录（2026-11-12）**：M11.0~M11.3 全部收官，tag `s11-done`。验收链：CPU `gi-tools selftest`（bounce 96/96 + afterglow）→ GPU atlas-dump ON/OFF（check-s11-bounce-atlas.py，合成图在本场景结构性盲已勘误）→ 稳态 6 档收敛 → 移动逐帧差分 ON≤OFF → Q11-2b 重开后 refgi 链 run.bat verify 全绿 + 老路径保形。下一步 S12 历史帧并轨。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M11.0 hashmap | 2~3 天 | `s11-hash` |
| M11.1 cell 更新 | 2 天 | `s11-update` |
| M11.2 查缓存+二跳 | 1~2 天 | `s11-bounce` |
| M11.3 稳定性验收 | 1~2 天 | `s11-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| **三处参考口径对比（先看这个）** | `docs/s10/GI-RESEARCH-NOTES.md` §2（2026-11-09 精读后的结构对照表） |
| cell 更新调度 | `Carrot/.../lighting/gi/update-cells.slang` |
| world cache 全套 | `bevy_solari/.../world_cache_update.wesl`、`world_cache_query.wesl`、`world_cache_compact.wesl` |
| hashmap 与回收坑 | kb/index.html#algo（04 章，含回收专条）；Carrot GI 长文（hashmap 实现心路） |
| 概念 | kb/index.html#lum（02 章 Surface Cache/Hash Grid 节） |

## 底线

- 多跳的「涌现证据」（渗透+余晖）就是验收本身——如果你在代码里显式写了「第二跳循环」，说明设计错了，正确形态是缓存反馈自然形成
- 本步与前步不同：**先 CPU 仿真验证 hashmap 语义，再上 GPU**——直接 GPU 调试 hash 链是时间黑洞
