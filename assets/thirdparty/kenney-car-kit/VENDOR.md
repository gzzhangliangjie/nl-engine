# assets/thirdparty/kenney-car-kit — Kenney Car Kit（车池）

- **来源**：https://kenney.nl/assets/car-kit （官方站挡 curl 403；经
  `cdn.cinevva.com/assets/packs/kenney/car-kit/<name>.glb` 直链获取，
  2026-11-14）。
- **许可证**：CC0 1.0 Universal（Kenney 资产官方声明，全站通用；cinevva
  转载无独立许可文本——以 kenney.nl 官方许可页为准，https://kenney.nl 支持
  声明「All assets are licensed under CC0」）。
- **入库范围**：25 辆中选 **8 种**（M-W3.0 车池，`kCarVariantNames` 与此目录
  一一对应，selftest 锁）：

  `sedan / sedan-sports / suv / taxi / police / van / truck / delivery`
  （均 `.glb`）

- **样张解剖**（2026-11-14）：多节点层级（body + 4 轮，node TRS 定位），
  colormap 图集 1 贴图，3~4K 顶点/辆。装载器 `mesh_io.cpp` 节点 TRS/层级全
  支持（§W3-PLAN 验证表），零引擎改动。
- **M-W3.0 嵌入变形（cinevva 获取路径特有）**：cinevva 单文件 glb 的
  image 是**外部 uri 引用**（`Textures/colormap.png`，原包分离结构残留），
  tinygltf 不加载外部文件 → viewer 渲染报「not 8-bit decoded
  RGBA-capable data」（M-W3.1 重拍 w1-m1 实锤）。处理 =
  `tools/embed_glb_textures.py` 把 colormap.png（Kenney 官方包 CC0 原图，
  经 sz-games 镜像获取，512x512 8-bit RGBA）追加进 BIN chunk 转嵌入式
  bufferView——8 辆全部处理，引擎零改动。嵌入后单文件自包含，与
  poly-streetlight 同形态。
- **静态停靠定位**：本轨车 = 摆放资产（位置/朝向/车型确定），运动系统归
  M-T0 交通轨（消费 roadnet.json）。
