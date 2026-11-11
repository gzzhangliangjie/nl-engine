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

- [ ] 每帧对每个活跃 cell：1 条**对光源**的 shadow ray（判可见性）+ 1 条**随机方向**射线（命中别的 cell → 把它的 irradiance 乘 albedo 加进自己——多跳反馈的机制本体）
- [ ] 随机方向每帧变化（帧号入 seed），多帧覆盖球面

**自检**：`cache-viz` 中 cell 颜色随帧号逐渐丰富（单帧噪声多帧收敛）。tag：`s11-update`。

## M11.2 探针查缓存 + 二跳 gather（1~2 天）

- [ ] S10 探针射线命中点改查世界缓存（不再用直光近似）；未命中 cell 才退回直光近似
- [ ] final gather 不变——多跳已经通过缓存反馈自然出现

**自检**：红白棋盘**颜色渗透出现**；关灯后余晖**自然衰减**（几帧内渐暗不是瞬间黑）。tag：`s11-bounce`。

## M11.3 稳定性验收（1~2 天）

- [ ] cell 生命周期打磨：回收延迟、新 cell 置信度预热（避免新 cell 一帧全亮闪）
- [ ] 摄像机快速移动压力测试

**自检（S11 验收）**：渗透+余晖稳定出现；cell 回收**无闪烁**；快速移动无明显 cell 涌现感。tag：`s11-done`，录屏 `docs/screenshots/s11_*.png`。

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
