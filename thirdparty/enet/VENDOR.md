# ENet — vendored 溯源（M-NET0，2026-11-13）

| 项 | 值 |
|---|---|
| 上游 | https://github.com/lsalzman/enet |
| 锁定版本 | **1.3.18**（`include/enet/enet.h` `ENET_VERSION` 1.3.18）@ master HEAD **`5a9c537fd464b3c6d3c55e1d3bd47588faf71b42`**（"fix erroneous NET_TO_HOST"，1.3.18 tag 后的修复提交，随浅克隆一并入库） |
| 获取方式 | `git clone --depth 1 https://gh-proxy.com/https://github.com/lsalzman/enet`（2026-11-13，gh-proxy 镜像实测可达） |
| 许可证 | **MIT**（`LICENSE` 原文随本目录入库） |
| C++ 标准 | 纯 C（C89 风格 API），本项目以 C 静态库编入（MSVC 14.38 / CMake 3.31.6 实测通过） |
| 构建形态 | 纯源码静态库 `nl_enet`（本目录 `CMakeLists.txt`，零构建期外部依赖；Windows 链接系统库 `winmm` / `ws2_32`，均为系统自带，不引入新运行时 DLL） |

## 入库子集与排除项

- **入库**：`include/enet/*.h`（9 个头）+ 8 个实现 TU（`callbacks.c` / `compress.c` / `host.c` / `list.c` / `packet.c` / `peer.c` / `protocol.c` / `win32.c`）+ `LICENSE` / `README` / `ChangeLog` + 本目录自有 `CMakeLists.txt`（上游 CMake 的 Windows 口径：`ENET_BUILDING_LIB` + `winmm`/`ws2_32`）。
- **剔除**：`unix.c`（只跑 Windows/MSVC）、上游构建脚手架（`CMakeLists.txt` 原版 / `Makefile.am` / `configure.ac` / `premake4.lua` / `enet.dsp` / `enet_dll.cbp` / `m4/`）、`docs/`、`.github/`、Doxygen 配置。

## 风险核查结论（选型硬闸对照，2026-11-13）

1. **开源无使用风险**：MIT，无 copyleft 传染；MIT 无显式专利授权条款，**不宣称绝对零风险**（硬闸第 2 条如实登记）。
2. **零构建期新依赖**：纯 C 源码直接编入，不联网、不装 SDK、不引新运行时 DLL。
3. **维护状态**：上游活跃（1.3.18 为 2024-04 发布；master 2025-2026 仍有修复提交）；ENet 是长期被广泛使用的成熟 UDP 网络库。
4. **已知能力边界（拍板依据，见 `docs/net/NETCODE-OPTIONS.md` §3/§5）**：连接管理 / 可靠+不可靠双通道 / 分片重组内建；**无加密 / 认证**（不阻塞 M-NET0~M-NET2；如将来开放对战需要，另拍板外挂加密层）。
5. **隔离纪律**：ENet 符号只允许出现在 `core/src/net/Net.cpp` 单一 TU（PIMPL，对齐 `core/src/nav/Nav.cpp` 先例）；业务代码一律经 `core/include/nl/net/` 薄封装，不暴露 `enet_` 类型。

## 升级约定

升级时同步更新本文件（版本 / commit / ChangeLog 摘要），并跑 `regression.py --only net-m0` 验证传输层行为无回归。
