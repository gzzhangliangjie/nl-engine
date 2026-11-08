# P8-AUDIO-PLAN — 3D 空间音频（A 类并行轨道，2026-11-08 立项）

> 轨道归属：`NON-RENDER-ROADMAP.md` 候选总表「3D 空间音频」行（★★★）。用户拍板（2026-11-08）：纳入引擎化整合，与寻路（`P8-NAV-PLAN.md`）/ 物理（`P8-PHYS-PLAN.md`）同批。
> **执行排期：放到场景整合之后**——听者（listener）与声源（source）的定位吃 P8 E1（ECS-lite 场景：变换层级 / 多物体）。先立项出计划，执行在 P8 E1 落地后启动。
> 届时按 `AGENTS.md` 并行轨道纪律新建 `feat/audio` worktree（junction 共享），验收走**工具版交代**（A 类：不进 `kStage`、不进渲染回归）。

## 路线分层（拍板口径）

参照 Carrot `engine/audio/`（1358 行）的分层——**集成层与解码器从零写，设备/混音/3D 定位用 OpenAL Soft**：

| 层 | 实现 | 依据 |
|---|---|---|
| 设备抽象 + 混音 + 3D 定位（距离衰减 / 方向 / HRTF） | **OpenAL Soft**（vendored，LGPL） | Carrot 同口径（`OpenAL.cpp/hpp` 封装）；3D 空间化与 HRTF 内建，避开 HRTF 从零的巨坑；LGPL 以独立编译单元方式链接（合规路径明确，版本锁死） |
| 集成层：AudioManager / AudioThread / SoundSource / SoundListener | **从零写** | Carrot 参照（抄思路不抄代码）；与 P8 E1 变换层级联动 |
| 解码器（WAV → MP3 → Vorbis） | **从零写** | Carrot `decoders/` 同路线，逐格式里程碑化 |
| 混响卷积 / 几何遮挡音频 | **不在范围**（Steam Audio 级特性，留待后续单独评估） | 范围失控防线 |

## 三处参考口径（2026-11-08 实读，执行各里程碑时按铁律重读本里程碑点名文件）

| 来源 | 内容 | 用法 |
|---|---|---|
| **Carrot** `engine/audio/`（1358 行） | `AudioManager`（设备/上下文）→ `AudioThread`（独立混音线程）→ `Sound/SFX/Music`（播放抽象）→ `SoundSource/SoundListener`（3D 定位端点）→ `decoders/`（WAV/MP3/Vorbis 自写解码） | **分层与节奏参照首选** |
| **UE** `Runtime/Engine/Audio*`（AudioMixer / SoundSourceGraph） | 工业终态：源图 / 子混音 / 空间化插件架构 | 只读对照；听者/声源与场景组件的耦合面作集成边界参考 |
| **bevy** | `bevy_audio`（rodio 封装）+ 生态 bevy_spatial_audio | rodio 线程模型可对照；不照搬 Rust 结构 |

## 里程碑阶梯（每步可见验收；编号 M-S，执行时按实际情况细化）

| 里程碑 | 内容 | 可见验收点 |
|---|---|---|
| **M-S0** | WAV 解码器（从零，PCM16/PCMf32） | 解码 dump → 波形 PNG 证据；与已知样本逐样本比对 |
| **M-S1** | OpenAL Soft vendored + 最小播放（单声源，正弦波/样本文件） | `audio-tools` 无头渲染：单声源输出到离线 buffer → 波形证据图 |
| **M-S2** | AudioThread 独立混音线程 + AudioManager 生命周期 | 多声源并发播放 dump；线程无竞态（压测用例） |
| **M-S3** | 3D 空间化：SoundSource/SoundListener + 距离衰减 | 场景两点声源，听者移动 → 衰减曲线 dump 证据；负向（超出 maxGain/maxDistance） |
| **M-S4** | MP3 / Vorbis 解码器（从零，逐格式） | 解码一致性比对（与参考解码器输出误差阈值） |
| **M-S5** | 场景集成：E1 场景物体挂声源 / 听者随主相机 | E1 场景（50 物体 + 8 灯）音频正确；展示台 tool 卡全绿 |

## 验收纪律

- 工具版交代：`audio-tools`（解码 / 离线渲染 / 空间化 dump）+ 展示台 `kind:"tool"` 用例（usage / 正向 / 负向）+ `AUDIO-TOOLS-GUIDE.md` 归档；证据图从 dump 真实输出生成（波形 / 衰减曲线）。
- **确定性**：固定种子与声源参数下，离线渲染 buffer 逐样本一致。
- 许可注意：OpenAL Soft 为 LGPL——以独立编译单元 + vendored 静态库方式链接并在 `thirdparty/` 保留其 license 声明（合规留痕）；版本锁死，升级单独走一轮验收。
- `core/audio/` 零图形符号；实时监听 UI 挂 viewer 层。
- merge 回 master 前过 §7.2 文档闸 + §7.5 关联文档同步。

## 排期约束（拍板留痕）

**执行不提前**：听者/声源定位吃 P8 E1 变换层级，提前做只能独立 demo 自嗨。启动时机 = P8 E1 验收通过之后；届时新建 worktree 并更新本文件与注册表。
