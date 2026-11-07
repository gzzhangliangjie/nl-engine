# thirdparty/meshoptimizer — vendored meshoptimizer 1.3

| 项 | 内容 |
|---|---|
| 来源 | `github.com/zeux/meshoptimizer` v1.3（MESHOPTIMIZER_VERSION 1030，2026-11-07 浅克隆） |
| 用途 | nanite-tools simplify 的**对照 oracle**（`meshopt_SimplifyLockBorder` 同口径对照行）；正式 QEM 实现在 `core/simplify`，core/viewer 不依赖本目录 |
| 许可 | `LICENSE.md`（MIT） |
| 改动 | 源码零改动；裁掉 demo/gltf/js/tools/extern/Makefile/CONTRIBUTING.md；顶层 `CMakeLists.txt` 为本工程自写（上游那份挂着 gltfpack 文件列表），`file(GLOB src/*.cpp)` 编静态库 |
| 构建 | `add_subdirectory(thirdparty/meshoptimizer EXCLUDE_FROM_ALL)`，target 名 `meshoptimizer` |
