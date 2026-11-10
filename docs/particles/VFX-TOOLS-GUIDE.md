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
    "rate":        [min, max],        // 粒子/秒（Effekseer interval 帧数折算）
    "life":        [min, max],        // 秒（Effekseer life 帧数 ÷ 60）
    "spawnPoint":  [x, y, z],         // 发射点（Point 型；随机范围 → exit 2）
    "velocityMin": [x, y, z],         // 初速盒 min（PVA velocity）
    "velocityMax": [x, y, z],         // 初速盒 max
    "accel":       [x, y, z]          // 常加速度（PVA accel min==max 才收）
  },
  "render": {
    "blend": "alpha | additive",      // Opacity/Blend→alpha，Add→additive
    "colorStart": [r,g,b,a],          // 0..255（SpriteAllColor Fixed/Random RGBA）
    "colorEnd":   [r,g,b,a],
    "size":       [sx, sy],           // 米（sprite quad 角点 × 节点缩放；Default 归一化 ±0.5）
    "fadeIn": s, "fadeOut": s,        // 秒（RendererCommon FadeIn/Out 帧数 ÷ 60）
    "texture": "path.png | null"      // 仅记路径；v1 运行时未采样（程序化软圆盘回退）
  }
}
```

坐标口径：Effekseer 默认 Y-up，与本引擎 sim 同轴（重力 -Y），v1 不做轴映射。

## 子集外模块（exit 2，实测触发过）

单 sprite 节点限定之外的一切：Model/Ribbon/Ring/Track 节点、多 sprite 节点、
生成 timing≠Continuous、MaxGeneration≠1、Burst≠1、发射位置 sphere/circle/line/model、
平移 Easing/FCurve/Nurbs/ViewOffset、PVA 加速度随机范围、缩放 PVA/Easing/FCurve、
材质文件（MaterialType≠Default）、Sub/Mul 混合、颜色继承、颜色 Easing/FCurve/Gradient/HSVA、
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

## 已知边界（M-PT2 候选）

- **贴图未采样**：.nfx 记录贴图路径但 v1 运行时画程序化软圆盘（样例贴图在 Effekseer
  仓库走 git-lfs，gh-proxy 镜像拉不到真二进制；stb_image 未单独 vendored）。
- **sizeY 记录未用**：billboard v1 用 X 值方形尺寸。
- rate min/max 区间取均值驱动模拟（区间本身留档在 .nfx）。
- 样例资产（assets/efx/）来自 Effekseer 仓库 Examples/Resources（MIT + 官方声明
  无使用限制）；`.efkefc` 是真二进制，贴图 PNG 在上游是 LFS 指针文件（无真体）。
