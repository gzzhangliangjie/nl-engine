# S9 详细开发路线图：GBuffer + 直光 + 阴影射线 + 离线参考模式

> 对应总路线 `docs/ROADMAP.md` 的 S9。预计 3 个里程碑、业余时间约 1 周。
> 两件事：① 每像素 1 条阴影射线的硬阴影直光（Lumen 的直光层）；② **64+ 采样/像素的离线参考模式**——它产出的「真值图」是 P5/S10~S12 调 GI 时唯一的裁判，本步的含金量全在这张图。

---

## M9.0 GBuffer 反解 + 直光框架（1 天）

- [ ] 从 S7 的 vis buffer 反解出 GBuffer（pos/normal/材质参数），供光追与 GI 复用
- [ ] 直光框架：BRDF（GGX + Lambert）+ 1~2 盏方向光，`--mode shadow` 的无阴影版先跑通

**自检**：直光结果与 S7 对照模式一致。tag：`s9-gbuffer`。

## M9.1 阴影射线（1~2 天）

- [ ] `--mode shadow`：直光采样点每像素发 1 条 shadow ray（朝光源方向），被挡→0，否则→BRDF
- [ ] 射线走 M8 的 TLAS；透明材质忽略（S9 不处理 alpha）

**自检**：硬阴影边缘干净；光源方向旋转阴影正确跟随。tag：`s9-shadow`。

⚠️ 常见坑：
- shadow ray 的 tMax 设到光源距离即可，别无限远（性能与正确性双赢）
- 自相交（shadow acne）：ray query 的 origin 沿法线偏移（bias）要用**世界尺度**的经验值 + 光源方向点积调

## M9.2 离线参考模式（2 天）

- [ ] `--mode reference`：每像素 64+ 条**余弦重要性采样**路径段（只算直光反弹一跳的聚合，噪声要求「极低」），离线积分写 EXR/PNG 存盘
- [ ] 分块/进度显示（一次性离线跑，几十秒可接受）
- [ ] 存盘带相机参数元数据（同机位可重跑）

**自检（S9 验收）**：硬阴影正确；参考图噪声极低（放大 400% 无明显颗粒）。这张图就是 P5 的裁判，**多存几个机位**。tag：`s9-done`，存 `docs/screenshots/ref_*.png`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M9.0 GBuffer+直光 | 1 天 | `s9-gbuffer` |
| M9.1 阴影射线 | 1~2 天 | `s9-shadow` |
| M9.2 参考模式 | 2 天 | `s9-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 直光+阴影射线的完整写法 | `Carrot/.../shaders/lighting/direct-raytracing.slang` |
| 采样与积分 | kb/graphics-handbook.html#path 的 G8（路径追踪器里程碑，同一套数学） |
| BRDF 细节 | kb/index.html#lum（02 章直光部分）；PBRT 第 14 章（当词典） |

## 底线

- 参考图工作流从今天建立：**每调一版 GI，同机位并排对比参考图**——没有裁判的调参全是玄学
- GBuffer 反解函数进公共头：S10/S12 全靠它
