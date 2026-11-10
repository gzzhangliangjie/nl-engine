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
    "texture": "path.png | null"      // M-PT2 起运行时采样：取 basename → assets/efx/textures/<basename>
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
  所有粒子同比例），实例流 32B 布局不变。ratio=1.0 时 shader 表达式逐位恒等，基线零漂移。

## 已知边界（M-PT3 候选）

- rate min/max 区间取均值驱动模拟（区间本身留档在 .nfx）；easing/渐变颜色仍不支持。
- 贴图无 mip 链（远处粒子闪烁，需时再加）；sizeY 为 per-emitter 定值比例
  （per-particle 随机 Y 需实例流扩字段，子集内无此需求）。
- 样例资产（assets/efx/）来自 Effekseer 仓库 Examples/Resources（MIT + 官方声明
  无使用限制）；`.efkefc` 是真二进制，贴图 PNG 在上游是 LFS 指针文件（无真体）——
  M-PT2 贴图改从 Effekseer 1.80.8 官方工具包 Sample/00_Basic（CC-0）取得。
