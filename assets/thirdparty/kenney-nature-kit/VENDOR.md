# assets/thirdparty/kenney-nature-kit — Kenney Nature Kit 2.1（树池）

- **来源**：https://kenney.nl/assets/nature-kit （官方站挡 curl 403；经 GitHub
  镜像 `ANAMASGARD/ClanCode` `public/assets/kenney_nature-kit/Models/GLTF format/`
  获取，gh-proxy 链路，2026-11-14）。
- **许可证**：CC0 1.0 Universal（随包 `License.txt` 原文入库）。
- **入库范围**：61 种树中选 **12 种形状代表**（M-W3.0 树池，`kTreeVariantNames`
  与此目录一一对应——city-tools 侧名字表与本目录内容是硬闸对齐，selftest 锁）：

  `tree_blocks / tree_cone / tree_default / tree_detailed / tree_oak /
  tree_palm / tree_pineDefaultA / tree_pineRoundA / tree_plateau /
  tree_simple / tree_small / tree_tall`（均 `.glb`）

- **样张解剖**（2026-11-14）：纯材质色模型（PBR `baseColorFactor`，零贴图
  依赖），单 mesh 1~2 prim，9~31KB/件。消费链零引擎改动：无贴图 prim 走
  `scene.cpp` 1x1 白占位回退 + `model.slang` factor×白 直渲。
- **前科规避记录**：kenney city-kit 为色板图集（无窗户图案）被 M-W1 实测否
  掉；nature-kit 为独立模型 + 纯材质色，非图集依赖，样张先看后定（W3-PLAN §2）。
- **镜像完整性**：非官方镜像，入库前已逐件 glTF JSON 头解剖验证（meshes/
  materials/images 计数与官方样张一致；CC0 许可文本与官方一致）。
