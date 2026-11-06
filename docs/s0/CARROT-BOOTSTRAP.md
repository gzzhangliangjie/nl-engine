# Carrot 从零搭建流程：S0 的参考教材

> 本地 `E:\UE_STUDY_2026\Carrot\` 有完整 1180 提交历史（2020-11-21 → 2026-09-12）。
> `docs/carrot-history.md` 挖的是 2023 年之后的 Nanite×GI；**本文挖的是 2020-11 起「从空仓库到能看画面」的那 10 天**。
> **仅做参考，不做逐行对齐**：Carrot 早期走了不少弯路（中途换 VulkanHpp、4 个月后才拆模块、相机坏 5 天、resize 修 4 次），见第三节「不复刻的弯路」。取它的节奏，不复制它的曲折。
> 所有 hash 都能直接 `git show <hash>` 看当时的完整 diff。命令一律在 Carrot 仓库根目录执行。

```
cd /e/UE_STUDY_2026/Carrot
git log --reverse --date=short --pretty=format:'%h %ad %s' HEAD | head -60   # 从头看顺序
git show 152edabe                                                            # 看三角形那次的全貌
git show --stat 46799759                                                     # 看 Init 到底建了什么
```

---

## 一、实况时间线（2020-11-21 ~ 12-11，前 21 天）

| # | hash | 日期 | 当时做了什么 | 当时的可见成果 |
|---|---|---|---|---|
| 1 | `46799759` | 11-21 | **Init**：16 行 CMake + 单 exe + `CarrotEngine.cpp` 186 行；GLFW 窗口、`VkInstance`、**validation layer debug messenger 从第一天就接上**；glfw/glm 源码直接 vendored 进 `thirdparty/` | 弹出一个空窗口 |
| 2 | `bc679592` | 11-21 | Swapchain creation + 自写 Vulkan 智能指针（RAII） | 有交换链，仍无画面 |
| 3 | `152edabe` | **11-22** | **Colored triangle**：CMake 里加 `add_spirv_shader()` 用 `glslc` 编 GLSL，产物挂进 target sources；IO 读文件；管线/renderpass/framebuffer 全在 `CarrotEngine.cpp` 里 | ⭐ **第二个工作日就出彩色三角形** |
| 4 | `146517a7` | 11-22 | Swapchain recreation（三角形出来**当天**就补 resize） | 缩放窗口不崩 |
| 5 | `703af81e`→`a0dbebbb` | 11-24~25 | 整体迁到 VulkanHpp，删掉第 2 步自写的 RAII 包装 | 行为不变，纯重构（弯路） |
| 6 | `7174a019` | 11-25 | 修退出时 DEP 异常、简化析构顺序 | 能干净退出 |
| 7 | `42163115` | 11-26 | Vertex buffer creation | 三角形顶点改从 buffer 读 |
| 8 | `d11508e3` | 11-27 | **Buffer / StagingBuffer 抽象**（498+ / 301-，第一次真正的抽象重构） | 上传逻辑可复用 |
| 9 | `98281cc1` `3c5eaca9` `6c859667` | 11-27~28 | Index buffer → UBO 创建 → UBO 使用 | 物体能动/能变换 |
| 10 | `1b6ed66d`→`6a30f711` | 11-28 | Depth buffer（一天内连修 4 次：只要一张 depth texture、正确选格式、**renderpass dependency + resize 不再崩**） | 有立体感，resize 稳 |
| 11 | `d7008069` `41e8134a` `19f0ad03` | 11-29 | 贴图加载 + image staging 上传 + **Mesh 抽象**（vertex+index 打包） | 带贴图 |
| 12 | `d25a1cc9` `441d0366` `04b7c4d2` | 11-30 | 基础模型加载、修颜色、**动态 viewport/scissor** | 能加载外部模型 |
| 13 | `2cb5eb33`→`3916e0af` | 12-01~04 | 从 shader stage 反推 descriptor layout、材质↔管线绑定、多材质、数据驱动 | 材质系统成形 |
| 14 | `21594df4` → `038c6d38` | 12-06 → **12-11** | 提交信息直接叫 **"Broken orbit camera"**，5 天后才 "Fix broken camera" | ⭐ 相机翻车（弯路） |
| 15 | `e5b52cb8` | 2021-02-20 | ImGui integration（**三个月后**，此时已有光追阴影和 ECS） | HUD |
| 16 | `e6bc5196` `4d06c0d3` `7658034a` `8c227e91` `8d7f508d` | 2021-03~11 | 单 `Engine` 类 → `engine/` 目录 → `VulkanDriver`/`VulkanRenderer` → `core` 模块拆分 | ⭐ **模块拆分是能跑 4 个月之后的事**（弯路） |

单功能 diff 规模（`git show --shortstat` 实测）：三角形 522 行、staging 抽象 498+/301-、模型加载 175 行、ImGui 400 行、模块化重构 915~1059 行。**卡住时看作者同类功能的规模，你的实现和它同量级；超一倍就是过度设计。**

---

## 二、值得取的节奏（不是代码风格）

1. **先出画面，再长抽象。** 第 3 个提交就有三角形，Buffer / Mesh / Material 抽象都是画面跑通后被重复逼出来的。S0 同理：M0.3 三角形是灵魂，M0.1 的三 target 保持薄，别在骨架阶段造架构。
2. **验证层第一天就位。** `46799759` 里 `debugCallback` + `checkValidationLayerSupport()` 就是 Init 的一部分——它是验收手段，不是后期加固。M0.1 就把日志/错误回调框架留好，M0.2 把 NVRHI device callback 接进来。
3. **新能力当天补 resize，并做成回归项。** `146517a7`（三角形当天）、`6a30f711`（depth 当天）都是好习惯；后面 `bb981ccb`、`2c87efe2` 又修两次，说明他没把 resize 做成回归——**我们做成 M0.2~M0.5 每步的回归项，预期一次修好，不预期修 4 次**。
4. **抽象等重复出现才做，一次 200~600 行。** 不要预支抽象。
5. **依赖先本地化，别卡在下载上。** Init 直接把 glfw/glm 源码塞进 `thirdparty/`。本机 GitHub 直连超时、`gh-proxy` 可用，这条正好合用。

### 底座不同，本来就得自己走

| 差异 | Carrot 当年 | 我们（NVRHI + Slang） |
|---|---|---|
| 模块边界 | 单 exe 单 `Engine` 类 | M0.1 就分 `core`/`renderer`/`viewer`：底座是 NVRHI、后面还有 S1~S14，提前隔离图形调用成本极低。**这是主动避开他 4 个月后 900+ 行重构，不是「不按流程走」** |
| shader 链 | GLSL + `glslc`；**Slang 是 2025-06 `98b0809d` 才引入的** | S0 起就是 `.slang`。CMake 结构可借 `152edabe` 的 `add_spirv_shader()`：`add_custom_command` + `MAIN_DEPENDENCY` + 产物挂进 target sources，只把 `glslc` 换成 `slangc` |
| 同步 / barrier | 手写 renderpass dependency、barrier、framebuffer | NVRHI 自动 barrier + dynamic rendering，M0.3 只需管 frame in flight = 2 的 fence。**`6a30f711` 那类坑由底座消掉，不必复刻样板** |

**不能继承的一点（底座不同）**：Carrot 现在的 `cmake_modules/ShaderSlangConfig.cmake` 从 `$ENV{VULKAN_SDK}/include/slang` 与 `lib/slang` 找 Slang，`BUILDING.md` 用 1.4.341.1 版 LunarG SDK——**它是裸 Vulkan 单后端项目，装 SDK 顺手把 slangc 一起拿到是合理的**。我们是 NVRHI 通用管线（DX12 / Vulkan 双后端），不能给某一个后端开特权：Slang 独立获取，Vulkan 头走 `KhronosGroup/Vulkan-Headers`，构建链不读 `VULKAN_SDK`。这条是 2026-11-06 架构校准推翻的旧结论（曾写成「装 SDK 自带 slangc，省一步」）。

---

## 三、不复刻的弯路（带 hash，看得见代价）

| 弯路 | 实况 | 代价 | 我们怎么做 |
|---|---|---|---|
| 底座选了又换 | `bc679592` 自写 Vulkan RAII → 3 天后 `703af81e`~`a0dbebbb` 全量迁 VulkanHpp、删掉自己写的类型 | 两次纯重构提交 | 底座 M0.0 一次定死（NVRHI + Slang），后续不换 |
| 模块拆得太晚 | `e6bc5196` → `4d06c0d3` → `8c227e91`，单 Engine 跑了 4 个月 | 单次 915~1059 行搬家式重构 | M0.1 就三 target，但各自只放最少代码 |
| 相机自己硬搓 | `21594df4` "Broken orbit camera" → `038c6d38` 修好，隔 5 天 | 一周里最不值的时间 | M0.4 直接用 glm + 现成轨道相机公式，先跑通再调手感 |
| resize 没进回归 | 前后修 4 次（`146517a7` `6a30f711` `bb981ccb` `2c87efe2`） | 每改一次渲染结构复发一次 | 门禁表里列为每阶段回归项 |
| HUD 拖到三个月后 | `e5b52cb8`（此时已有光追阴影和 ECS） | 之前调参全靠改代码重编 | M0.5 就接 ImGui，当验收仪器用 |
| 早期塞进无关依赖 | Lua / Mono / OpenXR / 音频 / 粒子编辑器在第一年陆续进来 | 体量膨胀，与渲染主线无关 | S0 只装窗口 + 渲染必需依赖 |

> 一句话：**他「先出画面、验证层第一天在、抽象被重复逼出来」值得学；他「选了又换、拆得太晚、相机硬搓、resize 不做回归」是学费，不用再交一遍。**

## 四、和 S0 六阶段的参照关系（非逐行对齐）

下表只回答「这一步大概在做什么、有哪些坑」，**不是必须对齐的进度表**；我们按自己的底座和阶段门禁走。

| 我们的阶段 | 可参照的 Carrot 实况 | 可取之处 | 不复刻 |
|---|---|---|---|
| M0.0 环境 | Init 的 vendored `thirdparty/` | 依赖本地化，先能构建 | 不装 Lua/Mono/OpenXR 等无关依赖 |
| M0.1 骨架+空窗口 | `46799759` | 第一天就有日志/错误回调、最小可跑 exe | 不搞他 4 个月后的目录层级 |
| M0.2 设备+swapchain | `bc679592` `146517a7` | 创建后立刻测 resize | NVRHI 接管 swapchain 样板 |
| M0.3 三角形+同步 | `152edabe` | CMake shader 编译链、当天出画面 | 手写 renderpass/barrier |
| M0.4 模型+贴图+相机 | `d11508e3` `d7008069` `d25a1cc9` | staging / Mesh 抽象的时机 | Assimp 全套、蒙皮、实例化、相机硬搓 |
| M0.5 ImGui+验收 | `e5b52cb8` | HUD 作为验收仪器 | 他拖了三个月，我们 S0 就要 |

## 五、可复用的现成资源（本机，零网络）

Carrot 的 `thirdparty/` 已 checkout，以下源码可直接复制到 `nl-engine/thirdparty/`，避开 GitHub 下载：

| 目录 | 用在 | 备注 |
|---|---|---|
| `thirdparty/glfw/` | M0.1 窗口 | 完整 `CMakeLists.txt` + `include/GLFW/glfw3.h`，`add_subdirectory` 即可 |
| `thirdparty/glm/` | M0.4 相机 | 数学库 |
| `thirdparty/tinygltf/` | M0.4 模型 | 单头文件方案，比 Assimp 轻 |
| `thirdparty/stb/` | M0.4 贴图 | `stb_image.h` |
| `thirdparty/imgui/` | M0.5 HUD | 注意 Carrot 用的是作者 fork，S0 只需基本功能 |
| `thirdparty/meshoptimizer/` | S1 起 | 先记着，S0 不用 |

外部代码研究（本机网络实测）：默认网络下 `raw.githubusercontent.com` 超时、`gh-proxy` 镜像 HTTP 200；开代理后直连也能通（0.94s）。两条都留着，哪条快用哪条：

```bash
# 镜像（默认网络）
curl -L "https://gh-proxy.com/https://raw.githubusercontent.com/NVIDIA-RTX/NVRHI/main/README.md" -o NVRHI-README.md
git clone --depth 1 --recursive "https://gh-proxy.com/https://github.com/NVIDIA-RTX/NVRHI.git"
# 直连（开了代理时）
curl -L "https://raw.githubusercontent.com/NVIDIA-RTX/NVRHI/main/README.md" -o NVRHI-README.md
```

注：大文件即使开代理也只有约 12KB/s，别在它们上面卡工期（CMake 官方 zip 实测单连接 ~12KB/s，改 8 段并发解决）。本项目不需要下载任何图形 SDK。

NVRHI 侧已核实的构建事实：CMake ≥ 3.10、C++17、推荐 `add_subdirectory(nvrhi)` 后链 `nvrhi` + `nvrhi_vk`；`NVRHI_WITH_VALIDATION` 默认 **ON**；Vulkan-Headers 可用 `NVRHI_FETCH_VULKAN_HEADERS` 自动拉（本机需把 git URL 换成 gh-proxy 前缀）——**这正是「Vulkan 头不用装 SDK」的依据**。

---

## 六、结论

Carrot 的**节奏**是「能跑的窗口 → 第二天出画面 → 被重复逼出抽象 → 验证层天天在」，这个值得参考；它的**曲折**是「底座换一次、模块晚拆四个月、相机坏五天、resize 修四遍」，这些不必复刻。S0 按自己的门禁表走：每阶段一个能亲眼看到的用例，验收通过才放行。
