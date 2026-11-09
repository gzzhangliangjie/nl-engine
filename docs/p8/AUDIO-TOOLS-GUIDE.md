# audio-tools 使用说明与结果归档（P8 M-S0 / M-S1 交代）

> **交代载体**：展示台网页 `tools/showcase/start.bat` → http://127.0.0.1:8787/ ——
> `audio-s0` 版本卡有 `kind:"tool"` 七用例（usage / selftest / probe / dump / check / 两条负向），
> `audio-s1` 版本卡另有七用例（usage / selftest / spatial / check / probe-b / 两条负向）。
> 本文是同一批输出的文本归档。M-S0 证据图 = `example/audio-s0/waveform.png` + `acceptance.png`；
> M-S1 证据图 = `example/audio-s1/attenuation.png` + `mixing.png` + `acceptance.png`。
> 上半部分是 **M-S0 收口时的原始归档**（保留历史口径不改），M-S1 追加节见文末。

## 定位

`nl/audio`（core 层，零图形符号）+ vendored miniaudio v0.11.26 的**独立验证载体**：
不链图形后端、零 ImGui 依赖，同一个 exe 承载两条路径——

- **离线**：`ma_engine_config.noDevice = MA_TRUE` + 显式 `channels=2` / `sampleRate=48000`，
  由 `ma_engine_read_pcm_frames()` 手动拉 f32 交错 PCM，固定参数下逐样本可复现；
- **实时**：`offline=false` 让 engine 自建默认播放设备，`play` 阻塞到声源播完。
  无设备时显式报错退出 1（对齐 bevy `audio_output.rs` 的 `stream: None` 优雅降级口径），不假装成功。

对象模型借 Carrot `engine/audio/` 的 Clip / Source / Listener / AudioWorld 分层；
**不抄 `AudioThread`**（miniaudio 内部已有混音，防双层线程）。M-S0 声源用
`MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION`：同步全解码保证离线确定性，
3D 空间化留 M-S1（有意简化，不是遗漏）。

## 退出码语义

| 退出码 | 场景 |
|---|---|
| 0 | 成功（selftest 全绿 / probe / dump / check / play） |
| 1 | 运行或断言失败：找不到 WAV、provenance 字段非法、逐样本不一致、无实时设备 |
| 2 | 用法错误：无参数、缺参、未知子命令 |

与 `nanite-console` 的 ExecCode 四档不同，本工具回到 nanite-tools 的 0/1/2 约定（展示台 `expectExit` 逐用例实锁）。

## 用法

```
audio-tools selftest                       21 条确定性断言，exit 0 即 PASS
audio-tools probe <wav>                    只读 WAV 头（PCM 16-bit mono）
audio-tools dump <wav> <outdir> [frames]   离线渲染 -> waveform.tsv + provenance.txt
audio-tools check <outdir>                 按 provenance 重渲染 + 逐样本比对
audio-tools play <wav>                     默认设备实时播放（阻塞）
```

复现全链：`example/audio-s0/run.bat`（依赖 `scripts/build-audio-tools.bat DX12` 先构建）。

## provenance 与「防假绿」

`dump` 写两个文件：`waveform.tsv`（`frame left right`，`%.9g` 保 float32 往返精确）
与 `provenance.txt`（format / engine / wav / sampleRate / channels / frames / gain / loop /
flags / `waveformHash` = tsv 字节流的 FNV-1a 64）。

`check` **不信任存下来的波形**：先逐字段校验 provenance，核 tsv 哈希，再按 provenance
里的参数**重新建 world 重渲染**，与 tsv 每一个 float 做位级 `==` 比对。这是照
`phys-tools check` 的教训——「check 只是把 dump 又跑一遍」是假绿。
`wav` 字段写相对仓库根路径（`NL_AUDIO_ROOT` 编译期注入），provenance 可跨 checkout 复现。

## 展示台七用例实跑归档（2026-11-09，DX12/Vulkan 双树同一个纯 CPU exe）

### 1. usage（无参数）→ exit 2

```
audio-tools - nl-engine P8 audio track (M-S0)
usage:
  audio-tools selftest                       deterministic unit tests (exit 0 = PASS)
  ...
exit codes: 0=ok 1=runtime/assert failure 2=usage error
```

### 2. selftest → exit 0

```
audio selftest (M-S0):
  ok   clip: WAV header anchors (48000/1/16/24000)
  ok   world: noDevice engine init
  ok   render: two independent offline renders succeeded
  ok   render: per-sample deterministic (bitwise)
  ok   render: mono clip expands to identical L/R
  ok   render: exponential decay envelope (head RMS > 4x tail)
  ok   check: re-render from provenance is per-sample identical
  ok   check: tampered provenance explicitly rejected (anti-false-green)
audio selftest: PASS (21/21)
```

### 3. probe 正向 → exit 0

```
sampleRate : 48000
channels   : 1
bits       : 16
frames     : 24000
duration   : 0.500000 s
probe: PASS
```

### 4. dump 正向 → exit 0

```
dump: frames=24000 rate=48000 ch=2 peak=0.595886 rms=0.122469 nonzero=23950
dump: waveform.tsv bytes=788665 hash=4293b40fe2f9a770
dump: PASS
```

（`assets/audio/test-tone.wav` 由 `scripts/make-audio-wav.py` 生成：440 Hz 正弦 × 指数衰减，
48000 Hz / mono / 16-bit / 0.5 s，纯标准库零随机，逐字节确定。）

### 5. check 正向 → exit 0

```
check: PASS (frames=24000, samples=48000, hash=4293b40fe2f9a770, per-sample identical)
```

### 6. 负向：缺失 WAV → exit 1

```
probe: FAIL cannot open WAV: .../test-tone.wav.missing
```

### 7. 负向：篡改 provenance → exit 1

```
check: FAIL provenance: sampleRate 44100 != engine 48000
```

`example/audio-s0/bad/provenance.txt` 故意把 sampleRate 写成 44100；check 在字段校验阶段
就拒绝，不会走到「拿错参数重渲染还报绿」。

## 证据图

| 图 | 数据源 | 肉眼核验 |
|---|---|---|
| `example/audio-s0/waveform.png` | `dump/waveform.tsv`（真实 `ma_engine_read_pcm_frames` 输出） | L/R 两栏 440 Hz 衰减包络清晰、峰值 0.595886、hash 与参数行完整、无截断无方框 |
| `example/audio-s0/acceptance.png` | `example/audio-s0/*.log`（七条命令真实 stdout 重定向） | 七节齐全、PASS/FAIL 可读、无中文方框（全 ASCII 渲染） |

`tools/make-audio-evidence.py` 只做「读真实输出 → 画像素」，不含任何手绘示意数据。

## 踩坑（本轮实遇）

1. **Python `// 1` 不转 int**：`FRAMES = SAMPLE_RATE * DURATION_S // 1` 得到 float，
   `range(FRAMES)` 直接 TypeError；改 `int(SAMPLE_RATE * DURATION_S)`。
2. **`git update-index` 的 `--stdin` 必须放最后**：`-z --stdin --skip-worktree` 报
   `option 'stdin' must be the last argument`，正确写法 `-z --skip-worktree --stdin`。
   共享 `.workbuddy` 的 5 个 memory 文件靠这一步 skip-worktree，否则 junction 内容变脏。
3. **provenance 曾写死本机绝对路径**：展示台换个 checkout 跑 `check` 就打不开 WAV。
   修 = `portableWav()`（写相对仓库根）+ `resolveWav()`（读时拼 `NL_AUDIO_ROOT`），
   宏由 CMake 注入；坏值/坏路径仍显式 exit 1。
4. **miniaudio 0.11.26 签名**：`ma_sound_init_from_file` 是 **6 参**（新增
   `ma_fence* pDoneFence`，传 nullptr）；`noDevice=TRUE` 时 `channels` 与 `sampleRate`
   **必须显式赋值**（官方头 1086-1099 行），否则 engine 无从推断格式。
5. **生命周期顺序**：`ma_sound` 必须先于 `ma_engine` uninit（声源节点挂在 engine 的
   node graph 上），`Impl::~Impl` 里显式遍历再 `ma_engine_uninit`。
6. **从 Bash 调 PowerShell 被安全策略拦**：共享目录 junction 的建立必须走独立的
   PowerShell 工具（`New-Item -ItemType Junction`），不能从 Bash 里 `powershell -Command`。
7. **证据图文本全 ASCII**：Consolas 无中文字形，历史 `M-SC0` 踩过中文方框，本轮预防性守住。

## 边界

M-S0 不做 3D 空间化（M-S1）、不做 console cvar / ImGui 面板（M-S2）、不碰 ECS 场景集成
（M-S3）；不动 `kStage`、不进渲染 `bytes/near` 回归总闸。`core/audio/` 零图形符号。

---

# M-S1 追加节（2026-11-09，tag `audio-s1`）

## M-S1 落码依据（三处参考 + 历史实读结论）

- **Carrot** `engine/audio/OpenAL.cpp:86` / `SoundSource.cpp:77` / `SoundListener.cpp`：
  只有 `setPosition` 转发 `alSource3f` / `alListener3f`，**无衰减模型参数**；衰减靠 OpenAL
  全局 distance model。→ 对象模型（Clip / Source / Listener / World 分层）可借，**衰减 API 不照抄**。
- **bevy** `audio_output.rs:330-380` / `audio.rs:56` / `sinks.rs:336-355`：`SpatialAudioSink`
  只有 emitter / 双耳位置 + `SpatialScale`，注释明写「无 HRTF、仅左右声像」。
  → 印证 3D 不必上 HRTF；M-S3 的 Transform 同步挂法可借。
- **UE** `Engine/Classes/Engine/Attenuation.h:16-24`（**只读不抄**，EULA / MIT 不兼容）：
  `EAttenuationDistanceModel` **6 种** + 4 种 shape + LPF + `AudioComponent.h:815 IsInAudibleRange`。
  工业终态，**不进本轮范围**，但超距判定口径借它。
- **miniaudio（落码依据）**：`ma_attenuation_model` 正好 **4 种**（`none / inverse / linear / exponential`），
  `inverse` 等价 OpenAL `AL_INVERSE_DISTANCE_CLAMPED`——与 Carrot 底座同源。公式对距离做
  `clamp(d, min, max)`，**超 maxDistance 不会自动静音**；故「超距静音」判据由 `nl/audio`
  薄层按 UE `IsInAudibleRange` 口径补剔除（见下「超距静音」）。
- `docs/carrot-history.md` / `docs/bevy-history.md` 全文无音频条目；本轮以三处源码实读为准。

## M-S1 新增 API（`nl/audio::AudioWorld`）

```cpp
struct AudioConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels   = 2;
    bool offline = true;
    bool spatialization = false;   // M-S1：默认 false -> M-S0 基线逐字节不变
};

enum class AttenuationModel { None, Inverse, Linear, Exponential };

// 声源
bool configureSource(id, gain, looping, AttenuationModel, minDist, maxDist, rolloff);
bool setSourcePosition(id, const Vec3&);
bool setSourceDirection(id, const Vec3&);
// 听者
bool setListenerPosition(const Vec3&);
bool setListenerDirection(const Vec3&);
bool setListenerUp(const Vec3&);
// 缓存（ma_resource_manager 承担 PCM 去重）
std::uint32_t registerClip(const std::string& wavPath);   // 返回 clipId，命中 / 未命中去重
bool createSourceFromClip(clipId, ...);
ClipCacheStats clipCacheStats() const;   // {clips, misses, hits, sources}
```

`spatialization=false` 时所有声源仍走 `MA_SOUND_FLAG_NO_SPATIALIZATION` 且
声源/听者位置被忽略——保证 M-S0 的 format=1 `waveform.tsv` 在 M-S1 下**逐字节不变**（已验证）。

## `spatial` 子命令与防假绿

```
audio-tools spatial <outdir>   21 距离点 × 4 模型 = 84 次独立离线渲染
                              -> attenuation.tsv + mixing.tsv + provenance(format=2)
audio-tools check <outdir>     按 format=2 provenance 重渲染全部采样点，格式化文本逐字比对
```

- **衰减扫描**：听者固定在原点面向 +Z；声源从 `z=0.5` 沿 +Z 步进到 `z=30`（21 点）。
  每点先 **warmup 512 帧**越过 `gainSmoothTimeInFrames=360`（miniaudio 默认 7.5 ms 增益平滑，
  否则量到的是过渡段），再读 **1024 帧** RMS 作为该距离增益。
- **混音**：声源 A（440 Hz，`test-tone.wav`）与声源 B（660 Hz，新增 `test-tone-b.wav`）
  并发；MIX 与 A+B 逐样本比较，`maxAbsDiff=0 / exactSamples=1024`。
- **provenance（format=2）**：`engine / wav / wavB / sampleRate / channels / distances /
  warmup / measure / rolloff / minDistance / maxDistance / attenuationHash（attenuation.tsv 字节流 FNV-1a 64）/
  mixingHash（mixing.tsv 字节流）/ cacheStats=clips:misses:hits:sources`。
- **`check` 防假绿**：不信任存下来的 tsv——从 provenance 重建全部 21×4 衰减采样 + 512 混音帧 +
  重新探测缓存统计，渲染成**格式化文本**，逐字比较。两条负向夹具分别在不同阶段被抓：
  - `bad/`（tsv 超距静音行被改成 0.1）：`attenuationHash` 校验阶段拒绝（exit 1）；
  - `bad-prov/`（只把 `maxDistance` 由 20 改成 10）：哈希合法，**重渲染后曲线不匹配**阶段才抓住（exit 1）。

## 4 种衰减模型与超距静音

miniaudio 原生 4 种 = Carrot 底座同源的 OpenAL distance model：

| 模型 | 公式（`d=clamp(dist,min,max)`） | 备注 |
|---|---|---|
| `None` | gain = 1 恒定 | 不衰减 |
| `Inverse` | `min / (min + rolloff·(d − min))` | 等价 OpenAL `INVERSE_DISTANCE_CLAMPED` |
| `Linear` | `1 − rolloff·(d − min)/(max − min)`（下限 0） | 线性 |
| `Exponential` | `(d/min)^(−rolloff)` | 幂律 |

`rolloff` 取 **0.5**（不是 1）：`rolloff=1` 时 `inverse` 与 `exponential` 数学恒等
（都是 `min/d`），证据图只剩 3 条可辨曲线，过不了「4 种模型」验收——这是本轮实踩的坑。
**超 maxDistance（=20）后三条模型不自动静音**：miniaudio 公式在 `d>max` 仍给极小正值。
`nl/audio` 按 UE `IsInAudibleRange` 口径补剔除：距离 > maxDistance 时增益**强制 0**（静音），
该口径写进 `UE-DIFFS.md`（D16）。

## 证据图（M-S1）

| 图 | 数据源 | 肉眼核验 |
|---|---|---|
| `example/audio-s1/attenuation.png` | `spatial/attenuation.tsv`（84 次真实离线渲染 RMS） | 4 条曲线分离（none 恒定 / inverse 最陡 / linear 线性 / exponential 幂律）；`z=20` 静音竖线可见；参数/hash 行完整；无截断无方框 |
| `example/audio-s1/mixing.png` | `spatial/mixing.tsv`（真实并发渲染） | A / B / MIX / RESIDUAL 四栏铺满全宽；MIX=A+B 残差为零平线；统计行完整 |
| `example/audio-s1/acceptance.png` | `example/audio-s1/*.log`（七条命令真实 stdout） | 七节齐全、PASS/FAIL 可读、无中文方框 |

## 踩坑（M-S1 实遇）

1. **`rolloff=1` 让 inverse≡exponential**：两者公式数学恒等（都是 `min/d`），证据图无法区分。
   改 `rolloff=0.5`，四条曲线分离。
2. **增益平滑 warmup**：miniaudio `gainSmoothTimeInFrames=360`（7.5 ms），直接读首帧量到的是
   过渡段而非稳态。每距离点先 warmup 512 帧再测 1024 帧。
3. **超距不自动静音**：miniaudio 衰减公式在 `d>max` 仍给极小正值，`nl/audio` 按 UE
   `IsInAudibleRange` 补 `gain=0` 剔除（D16 留痕）。
4. **第二素材频率必须不同**：`test-tone-b.wav`（660 Hz）与 `test-tone.wav`（440 Hz）频率不同，
   才能证明并发混音是两路独立信号相加而非复制；默认参数脚本不改 `test-tone.wav`（逐字节不变）。
5. **证据图宽度铺满**：`draw_channel` 原 `cols=min(w,len)` 在 512 帧时只占左 43%，视觉像被裁切；
   改为按画布宽铺满（24000 帧的 M-S0 图不受影响）。
6. **`auto* at(...)` 成员函数提取**：`const_cast<Impl*>` 在 4 个只读访问器里重复，抽成
   `Impl::at()` 内联成员，去掉 `const` 误用。
