# AddressSanitizer 诊断手册（MSVC + NMake 工具链）

> 来源：M7.1 收口实战（2026-11-09，tag `s7-pulling`）。当时 master visbuf 家族双后端静默崩溃（异常码 c0000374 堆损坏），常规日志插桩只能缩小到「init 成功、首帧炸」，ASan 一轮定位到行级根因。本文固化：开启方法、运行要点、报告解读、Windows 特有限制、经验纪律。

## 结论速览（TL;DR）

1. **开启**：CMake 临时加 `/fsanitize=address` + `/Zi`（编译）与 `/fsanitize=address` + `/DEBUG`（链接），**`/Zi /DEBUG` 是符号化的前提，不加只能看到裸地址**。
2. **运行**：把 `clang_rt.asan_dynamic-x86_64.dll` 拷到 exe 同目录；`export ASAN_OPTIONS=halt_on_error=1`。
3. **符号化**：MSVC 的 ASan 运行时在 Windows 上**自动走 dbghelp**——只要 PDB 在 exe 旁边，报告栈帧直接带 `文件:行号`，**不需要 llvm-symbolizer**（VS2022 默认也没装它）。
4. **重大限制**：MSVC 的 ASan **不附带 LeakSanitizer**，`detect_leaks=1` 在 Windows 上无效。**它抓的是错误访问（UAF/越界/空引用），不是内存泄漏**；泄漏检查要用 CRT debug heap 等替代方案（见 §5）。
5. **纪律**：ASan 是临时诊断件，用完即删，不入库；诊断期间跑烟测**禁用管道取退出码**。

---

## 1. 什么时候上 ASan（症状特征）

满足以下任一形态，先别插桩，直接开 ASan：

- 进程异常码 **c0000374**（堆损坏）——Windows 事件日志 `Application` 的 EventId 1000 里看 `ExceptionCode`。堆损坏的典型时序：**越界写在 init 期发生，崩溃却在稍后第一次碰坏数据时**，插桩标记常常「全没执行到」，极具误导性。
- 静默段错误（exit 139 / 0xC0000005），debug layer 与 NVRHI validation 零输出。
- 「初始化全部成功、首帧/退出时才崩」，且崩点虚调用或哈希表访问无业务逻辑解释。
- 崩点随无关改动「漂移」（比如合并挪了堆布局后旧代码开始崩）——这是潜伏越界被暴露的强信号，修合并没用，要抓第一现场。

反面案例（本轮实历）：崩点显示在 `ensurePipelines` 的 model 管线分支，看起来像「visbuf 代码改坏了」，实际根因在 init 参数错位（Scene 空跑）。**ASan 报告给的是现场不是根因，最后一公里仍要读代码闭环。**

## 2. 开启（CMake 临时块）

在根 `CMakeLists.txt` 的 C++ 标准设置之后加：

```cmake
# [TEMP 诊断] AddressSanitizer：抓堆损坏（c0000374）。用完即删，不入库。
set(NL_ASAN "ON" CACHE STRING "TEMP: enable AddressSanitizer")
if(NL_ASAN)
    add_compile_options(/fsanitize=address /Zi)      # /Zi：生成调试信息（符号化前提）
    add_link_options(/fsanitize=address /DEBUG)      # /DEBUG：产出 .pdb（符号化前提）
    message(STATUS "nl-engine: AddressSanitizer ENABLED (temporary diagnostic)")
endif()
```

要点：

- **`/Zi` + `/DEBUG` 不是可选项。** 本轮实测：不带它们时报告只有 `#0 0x7ff69c2440c9 in ???` 式裸地址；带上后重建（增量即可），同一崩溃点直接给出 `validation-device.cpp:1119` 行级符号。
- 不需要改生成器或加第三方依赖；MSVC 14.38（VS2022）自带 clang_rt 运行时。
- 走既有 `scripts/build-m0.2.bat DX12|VULKAN` 重建即可，注意 CMake option 变更会触发较多重编。

## 3. 运行与读报告

### 3.1 运行前置

```bash
# ① 运行时 DLL（每次干净重建后 exe 目录里没有，要重新拷）：
cp "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.38.33130/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll" build/DX12/

# ② 环境变量：
export ASAN_OPTIONS=halt_on_error=1
```

### 3.2 报告解读（以本轮真实报告为例）

```
==46840==ERROR: AddressSanitizer: access-violation on unknown address 0x000000000000 (pc ... T0)
==46840==The signal is caused by a READ memory access.
==46840==Hint: address points to the zero page.
    #0 0x... in nvrhi::validation::DeviceWrapper::validatePipelineBindingLayouts
           E:\...\validation-device.cpp:1119
    #1 0x... in nvrhi::validation::DeviceWrapper::createGraphicsPipeline ...:1249
    #2 0x... in nl::renderer::detail::Scene::ensurePipelines renderer\src\scene.cpp:2279
    ...
SUMMARY: AddressSanitizer: access-violation ... in nvrhi::validation::DeviceWrapper::validatePipelineBindingLayouts
```

- **`address 0x0` + `zero page`** = 空指针解引用（本轮：null shader 被塞进 pipeline 校验）。
- 常见错误类型对照：

| 报告 | 含义 | 典型根因（本项目语境） |
|---|---|---|
| `access-violation on 0x0` | 空指针 READ | 句柄没创建就被用；参数错位导致对象空跑 |
| `heap-use-after-free` | 释放后使用 | GPU 句柄活过 device 析构（shutdown 漏释放）；容器重排后留旧引用 |
| `heap-buffer-overflow` | 堆越界读写 | 数组下标、memcpy 长度、结构体布局不一致（ODR） |
| `SEGV on unknown address` | 野生指针 | 未初始化成员、虚表损坏 |

- **帧号从 #0 往上读**，第一个**本项目源码**帧通常就是根因入口（第三方库帧只是炸点）。
- 注意：崩溃时的栈是「第一现场」，但**堆损坏类问题的写坏点可能在更早**——UAF 报告会同时给 freed-by/allocated-by 两段栈，务必三段都读。

## 4. Windows 特有限制（必须知道）

1. **没有 LeakSanitizer。** MSVC 的 clang_rt.asan 不含 LSan，`ASAN_OPTIONS=detect_leaks=1` 静默无效。ASan 在本工具链上**只查错误访问，不查泄漏**。
2. **VS2022 默认没有 llvm-symbolizer**（`where llvm-symbolizer` 落空）。不需要装——运行时走 dbghelp + PDB 自动符号化；若某天确需离线符号化裸地址，用 WinDbg（`.symfix; .reload; ln <addr>`）或 VS 调试器附加。
3. 性能：ASan 下渲染帧率明显下降、exe/pdb 体积暴涨，**只用于诊断，禁跑基线对拍与性能口径**（GPU timer 数据作废）。
4. D3D12/NVRHI 的 GPU 侧错误（debug layer 报错、设备移除）ASan 看不见，它只管 CPU 侧堆与栈。

## 5. 内存泄漏检查（ASan 管不到的部分）

本项目语境下按优先级：

1. **GPU 句柄泄漏靠纪律不靠工具**：NVRHI 句柄（`*Handle`）全部 RAII 风格，但**必须挂在 Scene 存活期内主动置空**（`shutdown()` 里 `= nullptr`），否则句柄活到 device 析构之后，退出时报 UAF 或驱动侧泄漏。既有纪律：**新增 GPU 资源必须同步 `Scene::shutdown()`**（与「slot 级同步表必进 `createTargets()`」同级）。M7.1 实战：6 个 matResolve 句柄漏 shutdown，ASan 报 `heap-use-after-free` 于 `Scene::~Scene`——这类问题 ASan 能兜住，因为表现为 UAF 而非纯泄漏。
2. **CPU 堆泄漏用 CRT debug heap**（诊断期临时加）：

```cpp
#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <crtdbg.h>
// main 入口处：
_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
// 退出时 VS 输出窗口列出未释放块（含分配序号）；
// 配 _CrtSetBreakAlloc(n) 可在指定序号分配时断下。
```

3. 大规模排查才考虑外部工具（Deleaker / UMDH），本项目暂无此需求。

## 6. 实战案例索引（M7.1 轮）

| # | 症状 | ASan 报告 | 根因 | 修复 |
|---|---|---|---|---|
| 1 | `--mode visbuf/pulling` 双后端首帧静默崩（c0000374），normal/cull/dag/hiz 幸存 | `access-violation on 0x0`，栈顶 `validatePipelineBindingLayouts` ← `ensurePipelines` model 分支（scene.cpp:2279） | ECS 合并在 `Renderer::create` 的 hizMode/visbufMode 之间插入 `sceneMode`，`runNormal` 调用点漏传 → 实参整体错位一格，visbuf 以 `sceneMode=true` 空跑 Scene，三个模式标志全 false 掉进 model 分支解引用 null shader | `runNormal` 补 `/*sceneMode=*/false`（commit `2f04b33`） |
| 2 | 修完 #1 后退出时报 UAF | `heap-use-after-free`，栈指 `Scene::~Scene` 释放 GraphicsPipeline | 6 个 matResolve 句柄漏加进 `Scene::shutdown()`，PSO 活到 device 析构之后 | shutdown() 补 6 句柄 |

定位链条（可复用）：**git bisect 固定提交二分**（tag 绿 → 合并红）→ **ASan 拿行级现场** → 读代码闭环根因 → 修复后 ASan 全模式冒烟零报错 → 删临时块重建纯净版复验。

## 7. 经验与纪律（血泪浓缩）

1. **烟测禁用管道取退出码**：`viewer | tail` 后 `$?` 是 tail 的——本轮 19:06 的「exit 0」全是假阴性，一度误判行为翻转。要么无管道直跑，要么 `PIPESTATUS`。
2. **每次烟测前核对 exe mtime 与构建日志存在性**：构建命令 `&&` 短路会让后半（另一个后端）根本没跑，旧 exe 被当成新代码测；构建失败 exit 1 也曾被误读为「程序不崩」。
3. **插桩标记全没执行到 ≠ 没走到**：若 log 是 `std::endl` 逐行 flush 仍无输出，优先怀疑堆损坏延迟爆炸（写坏点在标记之前很远），直接上 ASan 别再加标记。
4. **ASan 临时块用完即删**：`NL_ASAN` CMake 块、运行时 DLL、pdb 都清掉，重建纯净版跑一轮对拍确认无行为差异再收口。禁止 ASan 版 exe 留在 build 目录里被后续流程误用。
5. **ASan 下不采信任何性能/画面口径**，只用于定位；验收数据一律出自纯净重建。
6. **固定 commit 上测 bisect**，工作区 stash 干净再切——本轮 bisect 结论全部在 detached commit 上重建验证，master 期间被并行会话推进也不受影响。
7. **崩溃≠最近改动引入**：先跑一个「与改动无关的老路径」对照（本轮 normal/cull 全绿）+ 纯净 HEAD 全量重建对照，再决定是查自己的 diff 还是查 master。
