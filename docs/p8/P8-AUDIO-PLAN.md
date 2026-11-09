# P8-AUDIO-PLAN — 3D 音效 + 游戏内音频调试（A 类并行轨道，2026-11-09 方案变更）

> 轨道归属：`NON-RENDER-ROADMAP.md` 候选总表「3D 空间音频」行。2026-11-08 立项（OpenAL Soft 路线），**2026-11-09 用户拍板整链换 miniaudio，本文件全量重写**（旧方案作废留痕见 §7）。
> **目标（用户两次追加拍板）**：① 能在场景里播放 3D 音效；② 能实现游戏内调试音效。不是「做一套音频中间件」，也不是复刻 UE/FMOD/Wwise 的音频内容管线。
> **性质**：A 类并行轨道（不碰 `kStage`、不进渲染回归），走工具版交代；`feat/audio` worktree 待建，验收走展示台 `kind:"tool"` 卡 + 真实输出证据图。
> **排期闸**：原「执行排 P8 E1 场景整合之后」不变。M-S0~S2 为纯逻辑（不依赖场景），是否提前开由用户解禁；M-S3 场景集成吃 E1（ECS-lite 变换层级）与 E1b 收口。

## 1. 方案：vendored miniaudio + 自写薄集成层

| 层 | 实现 | 依据 |
|---|---|---|
| 播放 / 混音 / 3D 空间化 / 解码 | **vendored miniaudio**（单 `miniaudio.c` + `.h`，PD 或 MIT-0 二选一，v0.11.26 / 2026-10-31） | 零外部依赖、纯 C、项目 `CMAKE_CXX_STANDARD 17` 直接编过；`ma_engine` 内建 WAV/FLAC/MP3 解码（dr_libs 已 amalgamated）、3D 空间化（位置/朝向锥/4 种距离衰减/Doppler/多 listener）、node graph；**不需要任何自写解码器、不需要独立混音线程** |
| 集成层 `nl/audio` | **自写薄封装**：AudioWorld / Source / Listener / Clip；暴露给 ECS 与 console 的最小 API | Carrot `engine/audio/` 分层抄结构不抄代码（对象模型）；状态所有权在 C++ |
| 游戏内调试 | console cvar + 独立 ImGui 声源面板 + 离线 PCM dump | 复用 master 现成 `nl/core/console.hpp`（`CVar` / `setOnChanged` / `ExecCode`）；UE `AudioDebug.cpp` 只读定字段口径 |
| 文件格式 | WAV / FLAC / MP3 内置；Vorbis 走官方 `custom_decoder(_engine)` 挂 stb_vorbis（可选，不进首版） | miniaudio 官方示例 `custom_decoder_engine`；项目 `thirdparty/stb` 已在场 |
| 不在范围 | HRTF / 双耳渲染、几何遮挡、EFX 混响卷积、Steam Audio、FMOD/Wwise、音频内容管线（事件/总线/RTPC/动态音乐） | 目标只要「3D 音效 + 能调试」；官方 manual 原文 HRTF/遮挡 not currently supported，不是 3D 音效的必要条件。FMOD/Wwise 是做游戏的事实标准，但商业闭源授权 + 构建链禁厂商 SDK + 本仓库无内容管线可喂，**目标分歧非方案优劣**（2026-11-09 用户讨论后仍拍板继续 miniaudio） |

**HRTF 升级路径**：官方现成 `engine_steamaudio` 示例（Steam Audio binaural node 接 `ma_engine`）。触发条件 = 将来明确要耳机双耳定位或几何遮挡时单独评估，不进本轨道。

## 2. 三处参考口径（2026-11-09 实读；各里程碑开工按铁律重读点名文件）

| 来源 | 内容 | 用法 |
|---|---|---|
| **Carrot** `engine/engine/audio/`（1358 行，本地实读） | `AudioManager`（loadSFX 常驻 / loadMusic 流式 / tick）→ `AudioThread`（`std::jthread` + 线程安全队列）→ `Sound/SFX/Music` → `SoundSource`（gain/pitch/looping/setPosition/isPlaying）→ `SoundListener`（updatePosition/Velocity）→ `decoders/`（**实为 `dr_wav.h`/`dr_mp3.h`/`stb_vorbis.h` 薄封装，非从零写**）；`AudioManager.cpp:138-148` 的 `mono_add_internal_call` 绑定面 | **抄对象模型与绑定形状，不抄底座**：① Source/Listener/Clip/SFX-vs-Music 分层；② 脚本绑定面与 `nl/script` Bindings 函数指针表同构；③ ⚠️ `AudioThread` **不抄**——miniaudio 内部已有混音，别叠两层线程（M-S2 开工前复核职责边界） |
| **UE** `Runtime/Engine/Private/AudioDebug.cpp` + `Classes/Components/AudioComponent.h`（只读） | 工业终态调试字段：`au.3dVisualize.ActiveSounds/Listeners/SpatialSources/VirtualLoops`、`au.Debug.SoundCues.*`（ShowPath/ShowDistance/Sort/Minimal）、`au.DumpActiveSounds`、`au.ClearMutesAndSolos` + 命令级 solo/mute | **只读不抄代码**（EULA）。M-S2 面板字段清单直接照它的口径定，不用自己想该显示什么 |
| **bevy** `bevy_audio`（`audio.rs` / `audio_output.rs`） | `SpatialListener` + `SpatialAudioSink` + `DefaultSpatialScale` + `update_listener_positions()`（每帧 Transform → 双耳位置）；注释明写「does not currently support HRTF … simple left-right stereo panning」 | M-S3「ECS Transform 每帧同步声源/听者」的组件挂法同代实证；也印证 HRTF 非 3D 音效必要条件 |

## 3. 现成封装参考（回答「不用从头造」）

- **miniaudio 官方示例**（`miniaud.io/docs/examples/`，2026-11-09 核对全清单）：
  | 示例 | 对应本轨道 |
  |---|---|
  | `engine_hello_world` / `engine_advanced` | M-S0 `ma_engine` 初始化、播放、参数面 |
  | **`simple_spatialization`** | M-S1 3D 定位：listener / 声源位置、衰减模型 |
  | `simple_mixing`、`resource_manager(_advanced)` | M-S1 多声源并发 + Clip 缓存 |
  | `node_graph`、`engine_effects` | M-S2 调试/效果节点插入 |
  | `custom_decoder(_engine)` | Vorbis 非内置的补法（挂自定义 decoder，不自写解码器） |
  | `simple_loopback`、`simple_playback_sine` | M-S0 离线确定性 dump / 波形证据 |
  | `engine_steamaudio` | 将来 HRTF 升级路径 |
- **本项目现成基建（零成本）**：`nl/core/console.hpp` 的 `CVar`/`setOnChanged`/`ExecCode`（console M-C0~C3 已 merge）→ `audio.volume/solo/freeze/dump` 直接挂；`nl/scene` 的 `World`/`Transform`（E1a 已 merge）→ M-S3 同步；展示台 tool 卡 + PNG 证据规范、`nl/script` Bindings 结构 → 交代物与脚本绑定不用另造。
- **vendored 纪律**：`thirdparty/miniaudio/` 只放 `miniaudio.c` + `miniaudio.h` + `VENDOR.md`（照 `thirdparty/jolt/VENDOR.md` 先例：来源 / 版本锁 / license 二选一声明 / 升级单独一轮验收）；锁 v0.11.26，不整仓拉 `examples/`/`extras/`；`#define MINIAUDIO_IMPLEMENTATION` 只在一个 TU。

## 4. 里程碑阶梯（4 步，每步可见验收；编号 M-S）

| 里程碑 | 内容 | 亲眼可见的验收 | 依赖 |
|---|---|---|---|
| **M-S0** | vendored miniaudio 接入 CMake；`nl/audio` 封装 AudioWorld/Source/Listener/Clip；单声源 WAV 实时播放 + 无设备离线渲染双通道（`ma_engine_read_pcm_frames()`）；`audio-tools` 工具（`play`/`dump`/`check`） | 波形 PNG 证据（真实 PCM dump 生成，不许手绘）；固定参数下离线 buffer **逐样本确定性**；usage/正向/负向 tool 卡 | 无，纯逻辑 |
| **M-S1** | 3D 定位：听者/声源位置朝向、4 种距离衰减模型、多声源并发、Clip 缓存 | 听者移动 → 衰减曲线 dump 证据图；负向用例（超 maxDistance 静音）；两声源并发波形叠加可验 | M-S0 |
| **M-S2** | **游戏内调试**：console cvar（`audio.volume/debug/solo/freeze/dump`）+ 独立 ImGui 声源面板（实体/位置/距离/增益/状态、独奏/静音、单步推进）+ 离线 dump | 调试面板截图（挂 console 那套独立 ImGui 窗口，默认关，**不动常驻 HUD** —— 否则触发全模式 HUD 基线重拍）；tool 卡 usage/正向/负向 | M-S1 |
| **M-S3** | 场景集成：ECS Transform 每帧同步声源、listener 随主相机；收口 merge | 50 物体场景里声源随物体移动（位置读回一致）；全量回归按需 + 收口硬闸 `wt_pool close` | E1 / E1b 收口 |

> **进度（2026-11-09）**：**M-S0 已收口**（tag `audio-s0`，`feat/audio` worktree）：vendored v0.11.26 + `nl/audio` 薄封装 + `audio-tools`（selftest / probe / dump / check / play）+ 展示台 `audio-s0` 卡 7 用例 + 波形/验收双证据图；DX12 + VULKAN 双构建树 `selftest 21/21`。离线 24000 帧 × 2 声道逐样本位级确定，`check` 从 provenance 重渲染比对（防假绿）。归档 `docs/p8/AUDIO-TOOLS-GUIDE.md`，台账 `docs/MILESTONES.md`。M-S1 起按阶梯推进。

可选（默认不做）：空间衰减球 gizmo（碰 viewer，排 E1b 收口后）、ScriptComponent 式 `OnTick` 音频脚本绑定、Vorbis custom decoder。

## 5. 验收纪律

- **工具版交代**：`audio-tools`（play / dump / spatial / check）+ 展示台 `kind:"tool"` 卡（usage / 正向 / 负向，`expectExit` 0/1/2）+ `docs/p8/AUDIO-TOOLS-GUIDE.md` 归档；证据图从工具真实输出生成（波形 / 衰减曲线 / 面板截图）。
- **确定性**：固定声源参数与帧数下，离线渲染 buffer 逐样本一致；`check` 必须从 dump 的 provenance 读参数（防假绿，照 `phys-tools check` 教训）。
- **零图形符号**：`core/audio/` 不得出现 NVRHI/ImGui/图形符号；ImGui 面板挂 viewer 层（M-S2）。
- **HUD 基线**：调试面板用独立窗口默认关，不改常驻 HUD（M-S2 硬约束）。
- **许可**：miniaudio PD / MIT-0 二选一，`VENDOR.md` 留痕；无 LGPL 重链义务、无 C++20 版本冲突。
- 收口前过 §7.2 文档闸 + §7.5 关联文档同步；`wt_pool close` 机械校验 + `release --done`。

## 6. 排期约束（拍板留痕）

- 原闸「执行排 P8 E1 场景整合之后」**不变**（听者/声源定位吃 E1 变换层级）。M-S0~S2 纯逻辑、不依赖场景，若要提前开工需用户单独解禁（照 ECS E1a / script / physics 三轨先例），本文件不擅自改闸。
- **2026-11-09 用户指令「开工 M-S0 建 feat/audio」= 提前解禁 M-S0~S2**（照 `PARALLEL-TRACKS.md` §7.1e 物理先例）：`feat/audio` worktree 已建于 `E:/UE_STUDY_2026/nl-engine-audio`（junction 共享 `.workbuddy` / `thirdparty/slang/dist`，skip-worktree），M-S0 已收口。**M-S3 场景集成仍 gate 在 P8 E1 / E1b 收口之后**，不随本轮解锁；M-S2 的 ImGui 面板仍只用 console 那套独立窗口、默认关、不碰常驻 HUD 与 kStage。
- 调试面板接 viewer 的部分与 E1b 渲染集成潜在撞车：M-S2 只用 console 那套独立 ImGui 窗口，不碰 `scene.cpp`/kStage/常驻 HUD。

## 7. 旧方案作废留痕（2026-11-08 立项 → 2026-11-09 全量替换）

> **旧路线**：OpenAL Soft（vendored LGPL）+ 自写集成层/AudioThread + 自写 WAV→MP3→Vorbis 解码器；6 步阶梯 M-S0（WAV 解码）→ M-S1（OpenAL 最小播放）→ M-S2（AudioThread 混音）→ M-S3（3D 空间化）→ M-S4（MP3/Vorbis 解码）→ M-S5（场景集成）；混响卷积/遮挡不在范围。

**2026-11-09 review 查出的三个问题**（当日日志已留痕）：

1. **硬错误**：「解码器从零写、Carrot decoders/ 同路线」是误读参照——Carrot 的 WAV/MP3/Vorbis 全是 `dr_wav.h`/`dr_mp3.h`/`stb_vorbis.h` vendored 单头库薄封装（dr_libs 官方「Public domain, single file audio decoding libraries」）；bevy_audio=rodio，UE=`ThirdParty/Vorbis`+`libOpus`。**三处参考无一从零写解码器**。
2. **合规**：OpenAL Soft 官方 README「LGPL 2 or later」，旧计划「独立编译单元 + 静态库链接（合规路径明确）」不成立（静态链接需提供源码/目标文件供重链）；默认 HRTF 数据集 SADIE II 另挂 Apache-2.0。
3. **构建**：OpenAL Soft 1.24.3 起 codebase 升 C++20（最新 1.25.1），项目 C++17；旧计划「版本锁死」未定版本。

**替换结论**：目标收窄为「场景 3D 音效 + 游戏内调试」后，OpenAL 的卖点（HRTF / EFX）用不上，成本（合规 / C++20 / 自写解码器 / 双层混音线程）全是净支出；miniaudio 单文件零依赖、内建 3D 与解码、无设备离线渲染直接支撑确定性 dump 与调试面板。原 6 步压到 4 步，砍掉：自写解码器、独立混音线程、M-S4、LGPL 合规、C++20 冲突、EFX/遮挡、Steam Audio。

**FMOD/Wwise 讨论留痕**（2026-11-09）：用户指出「做游戏确实 fmod 和 wwise 多点」——属实，商业游戏的事实标准，价值在成熟工具链 + 音频设计师工作流（事件/总线/RTPC/动态音乐/Profiler）。但本仓库目标是自写引擎内可播可调试、代码全在仓库、构建链禁厂商 SDK，且无音频内容管线可喂；这是目标分歧而非方案优劣。用户最终拍板「继续 miniaudio 路线」。若将来目标改成做游戏、要内容管线，再按 FMOD/Wwise 集成单独评估（含授权门槛）。
