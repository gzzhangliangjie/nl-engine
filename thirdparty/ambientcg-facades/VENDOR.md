# VENDOR — ambientCG Facade 立面纹理（M-W1.1 楼体贴图）

## 来源

- 站点：ambientCG（https://ambientcg.com）
- 资产：Facade 018A / Facade 001 / Facade 002（1K-JPG 包的 Color/albedo 贴图）
  - `tier1.jpg` ← https://ambientcg.com/get?file=Facade018A_1K-JPG.zip （红砖墙白框窗，低层住宅）
  - `tier2.jpg` ← https://ambientcg.com/get?file=Facade001_1K-JPG.zip （蓝灰规整玻璃幕墙窗格，中层办公）
  - `tier3.jpg` ← https://ambientcg.com/get?file=Facade002_1K-JPG.zip （深灰幕墙暖黄亮窗，高层塔楼）
- 下载日期：2026-11-13
- 提取：仅取各包 `*_1K-JPG_Color.jpg`（基色贴图），未改动像素；按楼体档重命名为 tier1/2/3.jpg。

## 许可证

- **CC0 1.0 Universal（Public Domain Dedication）**——ambientCG 全站统一许可。
- 官方许可文本：https://ambientcg.com/api/license/ （Creative Commons CC0 1.0 原文随本目录 `LICENSE` 入库）。
- 风险核查：CC0 = 不保留任何权利，商用/修改/再分发零条件；无专利/商标/诉讼记录。MIT 兼容（更宽松）。

## 使用方式

- `city-tools` generate 将本目录三张 JPG 拷贝为产物 `<out>/textures/tierN.jpg`，`blocks/city.mtl` 的 `map_Kd tierN.jpg` 按 basename 引用。
- `scripts/city2engine.py` 经 `obj2glb.py --maps` 将 JPG 嵌入 glb（mimeType image/jpeg）。
- 立面 UV 平铺单位（米/tile）由 `city-tools/main.cpp` 的 `kWindowUnitX/Y` 控制，按 Facade 纹理内窗排数实测校准。

## 升级

- 换资产时同步更新本文件下载 URL/日期/命名映射；JPG 不入构建期依赖（运行时字节拷贝）。
