# VENDOR.md — nlohmann/json

- **库**:nlohmann/json(single-header "JSON for Modern C++")
- **版本**:3.10.4(与仓内 `thirdparty/tinygltf/json.hpp` 内嵌同款逐字节同源,原样复制)
- **上游**:https://github.com/nlohmann/json
- **许可证**:MIT(MIT 无显式专利授权条款,此事实如实写明,不宣称"绝对零风险");
  许可声明见 json.hpp 头部(Loran T / Niels Lohmann, SPDX MIT)
- **C++ 标准**:C++11 起,本仓按 C++17 编译
- **入库子集**:仅 `json.hpp` 单头(3.10.4 单头形态);不含官方仓库其余文件
- **排除项**:无(单头即全集)
- **消费方**:`modules/citygen/`(M-CG0 Overpass `[out:json]` 解析)。第三方类型不进公开头文件,
  仅在 src/*.cpp 内部 include(选型闸第 5 条「nl 薄封装隔离」)。
- **风险核查**:零构建期新依赖(header-only、不联网、无 DLL);MIT 与本仓 MIT 兼容;
  官方仓库活跃维护,单头形态即官方分发物,无停维护风险。
