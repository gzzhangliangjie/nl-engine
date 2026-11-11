# vfx-tools 使用指南（M-PT1 归档）

> `vfx-tools` = Effekseer `.efkefc` → 自定义 `.nfx` v1 离线转换器（B 类轨道工具，
> 路线 B 用户拍板 2026-11-10）。运行时（viewer/renderer）**零 Effekseer 依赖**：
> 转换期把 Effekseer 参数面折算进 NFX1 子集，运行时只解析 JSON。
> 解析层 = vendored Effekseer core（MIT，`thirdparty/effekseer/VENDOR.md`）。

## 用法

```
vfx-tools convert <in.efkefc> <out.nfx>    # 转换（exit 0 成功 / 1 解析失败 / 2 用法或子集外）
vfx-tools selftest                          # JSON 往返 + 坏 magic + SKFE 探针自证
```

退出码约定（对齐展示台 expectExit）：**0** = 成功；**1** = 断言失败或文件损坏解析失败；
**2** = 用法错误**或子集外模块显式报错**（stderr 打印逐条原因清单，禁静默丢弃）。

## .nfx v1 子集（NFX1）

```json
{
  "magic": "NFX1",
  "source": "CullingSprite.efkefc",
  "efkVersion": 1500,
  "emitter": {
    "rate":        [min, max],        // 粒子/秒（Effekseer interval 帧数折算）；M-PT4 E1 起
                                       // 运行时按帧随机采样该区间（rateSeq_ 专用 pcg 序列）；
                                       // min==max 定值路径 IEEE 恒等零漂移
    "life":        [min, max],        // 秒（Effekseer life 帧数 ÷ 60）
    "spawnPoint":  [x, y, z],         // 发射点（Point 型；随机范围 → exit 2）
    "velocityMin": [x, y, z],         // 初速盒 min（PVA velocity）
    "velocityMax": [x, y, z],         // 初速盒 max
    "accel":       [x, y, z],         // 常加速度（PVA accel min==max 才收）
    "ang":         [min, max],        // M-PT5 E5 可选字段：初始角（弧度；口径 =
                                      // vendored Effekseer runtime 直存弧度）
    "angVel":      [min, max]         // M-PT5 E5 可选字段：角速度（弧度/秒）
  },
  "render": {
    "blend": "alpha | additive",      // Opacity/Blend→alpha，Add→additive
    "colorStart": [r,g,b,a],          // 0..255（SpriteAllColor Fixed/Random RGBA）
    "colorEnd":   [r,g,b,a],
    "size":       [sx, sy],           // 米（sprite quad 角点 × 节点缩放；Default 归一化 ±0.5）
    "fadeIn": s, "fadeOut": s,        // 秒（RendererCommon FadeIn/Out 帧数 ÷ 60）
    "texture": "path.png | null",     // M-PT2 起运行时采样：取 basename → assets/efx/textures/<basename>
    "flipbook": {                     // M-PT3 可选字段（缺省/缺字段 = 无；v1 老文件零改动兼容）
      "frameLength": 12.0,            // 每帧时长，单位 = Efke 帧（1/60s）
      "countX": 4, "countY": 2,       // 网格划分数
      "loop": 1,                      // 0=once（末帧驻留）1=loop 2=reverse
      "offset": [0.0, 0.0],           // UV 起始偏移（V0 = 贴图顶部）
      "cell": [0.25, 0.5]             // 单帧 UV 尺寸
    },
    "colorCurve": {                   // M-PT4 E2 可选字段（缺省 = 无 → colorStart/colorEnd 线性 lerp 原路径）
      "colorKeys": [[t,r,g,b], ...],  // t ∈ [0,1] 非降序；rgb 0..255（与 colorStart 同单位，运行时 /255）
      "alphaKeys": [[t,a], ...]       // a 0..255；两数组均非空，采样端点 clamp 分段线性
    },
    "sizeYRange": [min, max]          // M-PT5 R1 可选字段：per-particle Y 绝对尺寸
                                      // 区间（米）；缺省 = 无 → per-emitter size[1]
                                      // 口径不变。启用时 size[1] 语义由本字段承担
  }
}
```

坐标口径：Effekseer 默认 Y-up，与本引擎 sim 同轴（重力 -Y），v1 不做轴映射。

## 旋转 / sizeY 来源映射（M-PT5 E5/R1）

| Effekseer 参数面 | 收子集 | .nfx 落点 | 子集外 |
|---|---|---|---|
| Rotation None | 全收 | 缺省（无字段） | — |
| Rotation Fixed `Position.Z` | Z 定值 | `emitter.ang = [Z, Z]` | X/Y 非 0 exit 2（billboard 只认屏幕平面） |
| Rotation PVA `rotation/velocity.z` | Z 区间 | `emitter.ang` / `emitter.angVel` | rotation/velocity X/Y 区间非 0、acceleration 非 0 exit 2 |
| Rotation Easing/AxisPVA/AxisEasing/FCurve/RotateToViewpoint/Velocity | — | — | exit 2 |
| Scaling None/Fixed | 全收 | `render.size` | RefEq exit 2 |
| Scaling PVA `position.y` 区间 | y 随机 | `render.sizeYRange` | X 区间非定值、velocity/acceleration 非 0 exit 2 |
| Scaling Easing/FCurve | — | — | exit 2 |

运行时口径（M-PT5 实例流 64B）：`progressMisc.z = angle`（弧度，每粒子 spawn 派生 +
step 积分 `angle += angVel*dt`）、`progressMisc.w = 该粒子 Y/X 比`、VS 弃 CB 标量
`g_params.z` 改 per-instance 读。**零漂移双保险**：旧随机槽 `s*5u+{0..4}` 公式一字
不动，新槽 `s*7u+0x100u+{0,1,2}` 输入空间永不相交；`angle=0` 时 VS 旋转公式
IEEE 恒等、`w` 定值路径逐粒子复制旧 `ratioY` 同一 float——pt0~pt3 基线逐字节零重拍。

## colorCurve 来源映射（M-PT4 E2）

| Effekseer ColorAll 类型 | 处理 | 说明 |
|---|---|---|
| Fixed / Random (RGBA) | → colorStart/colorEnd（原 M-PT1 口径不变） | Random 逐粒子 min/max 折两端点 |
| Easing (RGBA, start/end 定值) | 烘焙 17 关键帧：`d = A·t³ + B·t² + C·t`（easing_color::setValueToArg 同式），`Color::Lerp(start,end,d)` 逐通道 clamp 0..255 | 对 8bit 输出量化误差可忽略；colorStart/colorEnd 折自首末键 |
| Gradient | 直映射：ColorKeys → colorKeys（intensity 全等时折进色值，精确）、AlphaKeys → alphaKeys；0 alpha 键 = 常量 255 | 非均匀 intensity = 二次曲线无法线性精确 → exit 2 |
| Easing HSVA / per-particle 随机 start/end / FCurve / Gradient 乱序 key | exit 2 | 单发射器关键帧无法精确表达 |

## 子集外模块（exit 2，实测触发过）

单 sprite 节点限定之外的一切：Model/Ribbon/Ring/Track 节点、多 sprite 节点、
生成 timing≠Continuous、MaxGeneration≠1、Burst≠1、发射位置 sphere/circle/line/model、
平移 Easing/FCurve/Nurbs/ViewOffset、PVA 加速度随机范围、缩放 PVA/Easing/FCurve、
材质文件（MaterialType≠Default）、Sub/Mul 混合、颜色继承、
颜色 FCurve / Easing HSVA / Easing 随机 start/end / Gradient 非均匀 intensity / Gradient 乱序 key
（M-PT4 E2 起收 Easing 定值 RGBA 烘焙 + Gradient 均匀 intensity 直映射，见上表）、
UV 动画、非 Billboard、旋转 PVA/Easing/FCurve、力场、碰撞、音效、GPU 粒子模块、
KillRules、Falloff、AlphaCutoff、动态参数引用（RefEq）、magnification≠1。

## 运行时加载端

```
viewer.exe --nfx <file.nfx>       # 隐含 --particles；失败退 legacy 喷泉并打 error 日志
```

- `Particles::loadNfx`（renderer/src/scene.cpp）：NFX1 JSON → 模拟参数面
  （box 初速逐轴 lerp + 常加速度 nfx 路径；**legacy 喷泉代码路径逐位不动**，M-PT0 基线零回归）。
- additive 混合走懒建变体管线（同一 shader/bindingLayout，仅混合方程不同）。
- 加载成功即清空模拟状态（spawnSeq 归零 = pcg 确定性流重启）。
- 固定步长 1/60s 纪律不变：nfx 模式同样 run-to-run PNG 逐字节同（实测 MD5 7fd2a033…）。

## 贴图采样端（M-PT2 起）

- **路径解析**：`.nfx` 的 texture 字段（.efkefc 内部相对路径）运行时只取 basename，
  解析为 `<repo>/assets/efx/textures/<basename>`（NL_ASSET_DIR 注入的是 assets/models，
  代码里取其父目录拼 efx/textures）。找不到文件/解码失败 → log 一条 deviceError 并
  **回退程序化软圆盘**（不炸帧；deviceErrors=1 会卡验收闸，属预期——贴图资产缺失应修资产）。
- **解码与上传**：stbi_load_from_memory 强制 4 通道 RGBA8（声明头 thirdparty/stb/stb_image.h，
  实现复用 nl_core mesh_io.cpp 的 tinygltf 内嵌展开，全工程仅一份）。纹理 RGBA8_UNORM，
  **原样采样不做 sRGB**（与引擎全链无 gamma 口径一致）。建纹（ensure，device 侧）与像素
  上传（draw 首帧 writeTexture，cmd 侧）分离——建纹时无 cmd list 可用。
- **绑定**：t1 纹理 + s0 Linear clamp 采样器；无贴图时绑 1x1 白 dummy 过 NVRHI 校验，
  PS 用 CB `g_params.y` 开关走软圆盘路径（0 = 软圆盘逐字 M-PT0 原表达式）。
- **sizeY**：`render.size` 的 sy 生效——**Y/X 比走 CB 标量 g_params.z**（单发射器子集
  所有粒子同比例），ratio=1.0 时 shader 表达式逐位恒等，基线零漂移。
- **mip 链（M-PT3）**：CPU box-filter 逐层 2×2 均值（纯整数/加法 = 逐位确定），全
  mipLevels 建纹 + draw 首帧按层 `writeTexture` + sampler `setMipFilter(true)`。远处粒子
  抗闪烁。基线影响：粒子屏幕尺寸 < 贴图尺寸时会采样进高层 mip（如 tex-flame 0.25m 高
  粒子），该类基线随特性演进重拍。
- **flipbook（M-PT3）**：帧号折算全在 CPU（Effekseer GetUV 同式纯整数；`elapsed×60+0.5f`
  消浮点截断抖动），实例流 48B `progressMisc.xy` = (frameX, frameY)；VS 统一无 branch
  公式 `flipUv = uvRect.xy + uvRect.zw*(frameXY + localUv)`，CB 128B `g_uvRect`——非
  flipbook 时 uvRect=(0,0,1,1)，IEEE 精确恒等于 M-PT2 texUv（零漂移）。子集外：
  StartFrame 随机 / LERP 帧间插值 / FrameLength≤0 或极大值 → 转换器 exit 2。

## 已知边界（历史快照）

> 本节为 M-PT2/M-PT3 收口时点快照；**扩展点的活登记表见 `EFFECTS-STATUS.md`**（状态/改动面/触发场景逐项标注）。

- ~~rate min/max 区间取均值~~（M-PT4 E1 已落地：区间按帧随机采样，span=0 IEEE 恒等）；
  ~~easing/渐变颜色仍不支持~~（M-PT4 E2 已落地：colorCurve 可选字段，见来源映射表）。
- ~~sizeY 为 per-emitter 定值比例~~（M-PT5 R1 已落地：sizeYRange 可选字段 per-particle 随机，
  实例流 64B per-instance 读）；~~旋转 billboard 不支持~~（M-PT5 E5 已落地：屏幕平面 Z 分量
  Fixed/PVA，Easing/Axis*/FCurve/RotateToViewpoint/Velocity 仍 exit 2，见来源映射表）。
  flipbook 的 LERP 帧间插值 / StartFrame 随机未支持（M-PT3 起 mip 链已落地，其余见登记表）。
- 样例资产（assets/efx/）来自 Effekseer 仓库 Examples/Resources（MIT + 官方声明
  无使用限制）；`.efkefc` 是真二进制，贴图 PNG 在上游是 LFS 指针文件（无真体）——
  M-PT2 贴图改从 Effekseer 1.80.8 官方工具包 Sample/00_Basic（CC-0）取得。
