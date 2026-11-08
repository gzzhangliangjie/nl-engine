# anim-tools 指南（M-A 轨道工具版交代）

> A 类并行轨道（`PARALLEL-TRACKS.md` §2.4 / `docs/action-system/ACTION-PLAN.md`）。
> 不进 `kStage`、不进渲染回归总闸；交代 = 展示台 tool 卡实跑 + 真实输出生成的证据图 + 本文归档。

## 用法

```
anim-tools selftest curve|blend|statemachine|blendspace   # 确定性单测
anim-tools sample                                          # 确定性演示输出（证据图数据源）
```

退出码：`0` 成功 / `1` 断言失败 / `2` 用法错误。工具只链 `nl_core`（零图形 API），构建：

```
scripts\build-anim.bat anim-tools    # vcvars + NMake（沙箱 reg 黑名单下的 INCLUDE/LIB 手工补丁，见 M-A1 踩坑）
```

## selftest 覆盖（全部确定性数值断言，两次运行逐位一致）

| 用例 | 覆盖 |
|---|---|
| `curve`（M-A1） | lerp/slerp 数值锚点（45°/22.5° 四元数）、最短路径翻转不变、近平行 nlerp 退化、Step 保持前 key、常量/空轨道、wrapTime 负回绕、validateClip 六负向 |
| `blend`（M-A2） | lerpPose 锚点 w=0/0.5/1、blendClips 跨 clip 混合、加法 quat 相对叠加（0/45/90/180°）、骨架不一致拒绝 |
| `statemachine`（M-A3） | 10 步 loco 场景逐值断言、回退 z 连续 + UE 缩时、validate 五负向、双跑一致 |
| `blendspace`（M-A4） | 段权重锚点、域外 clamp、单 sample 退化、validate 五负向、参数域扫掠 |

## sample 输出与证据图

`anim-tools sample` 输出两段确定性数值表（ASCII）：

1. **blend space 扫掠**：param 0→2 步长 0.25（t=0.5）——`param z (seg i0->i1 w=…)`；
2. **状态机 10 步场景**：`step speed state alpha z`（dt=0.1，speed 输入驱动 idle→walk→run→回退→idle）。

`python tools/anim_evidence.py` 消费该输出画 `example/ma-track/evidence.png`（960×540，纯标准库
zlib+struct 写 PNG，5×7 位图字体，不手绘示意）；上面板 = blend space 分段线性 z(param)，
下面板 = 状态机 z(step)（红）+ transition alpha（绿点）+ 状态色带。**两次生成逐字节一致。**

## 复现

```
example\ma-track\run.bat    # 四自测 + 负向(exit 2 校验) + sample + 证据图再生成，ALL GREEN 才 exit 0
```

ctest（构建目录内）：`ma1-anim-curve-selftest` / `ma2-anim-blend-selftest` /
`ma3-anim-statemachine-selftest` / `ma4-anim-blendspace-selftest` / `ma1-anim-unknown-case`（WILL_FAIL 负向）。

## 踩坑存档（详版见 git log 与当日记忆）

1. 沙箱 reg.exe 黑名单打断 vcvars 注册表探测 → rc/mt/LIB 缺失；`build-anim.bat` 手工补 SDK bin + INCLUDE/LIB。
2. validateClip 曾把空通道判非法 → 仅 rotation 轨道的骨骼被拒 → sampleClip 空 poses → 自测 `poses[0]` 段错误（逐用例进度输出定位）。
3. 自测期望值手算错误三连（漏算 crossfade 期间 fromTime 推进、sin(11.25°) 常数笔误、断言时机在场景结束后）——实现全对、期望全错，逐值核算修正。
4. `blendPoses` 输入输出同容器混叠（先 clear 毁输入）——evaluate 改走临时缓冲。
5. `t=1.0` 撞 wrapTime 循环归零语义（t=duration → 0）——测试时点避开端点。
6. NMake 时间戳陷阱复发：obj 在 `CMakeFiles/<target>/<target>/` 子目录，删错层不重编（M2.3a 坑 3 同款）。

## M-A4b：骨架 FK + rigged glTF 入口（2026-11-08 追加）

**新增模块**（`core/include/nl/anim/` + `core/src/anim/`）：

| 文件 | 内容 |
|---|---|
| `skeleton.hpp/.cpp` | `Skeleton` 扁平拓扑表（boneNames / parentIndices / restPose，父先于子）+ `validateSkeleton` + `localToModel`（纯 (T,R) FK，out 允许与输入同容器）；`scale` 口径后置（与 BoneTrack 一致） |
| `gltf_anim.hpp/.cpp` | `loadGltfAnim(path) -> Skeleton + AnimClip[]`：scene 全节点 DFS 收表；动画通道 LINEAR/STEP、rotation 归一化；SCALE 通道 / CUBICSPLINE 采样器**显式拒绝**；⭐静态 TRS 填单关键帧轨道（空轨道=零/identity 常量语义，不能拿来兜 rest） |
| `anim_math.hpp` 追加 | `rotVecByQuat`（q v q⁻¹ 优化式，FK 用）、`quatFromAxisAngle`（锚点构造） |

**资产**：`assets/models/RiggedChain.glb`（1504 B）由 `tools/make_rigged_glb.py` 程序化生成——3 骨骼链 hip→spine→head（平移 (0,1,0)/(0,0.5,0)/(0,0.5,0)）+ 单个 sway 旋转动画（duration 2.0 s，关键帧 [0, 0.5, 1, 1.5, 2] s，绕 Z 相位错格）。全部关键帧手算可断言。

**自测**：`anim-tools selftest skeleton [rigged.glb]`
- FK 锚点：hip 绕 Z 90° → spineWorld (-0.5, 1)、headWorld (-1, 1)；rest 直加 → head y=2；尺寸不匹配 / 拓扑序破坏 / 重名负向。
- 资产链：t=0.5 hip=+30°（quat y=sin15°=0.25881905）、spineWorld (-0.25, 1.4330127)、headWorld (-0.5, 1.8660254)；链长守恒 reach=1；wrap t=2.5 == t=0.5 逐位；两次加载逐位一致；缺文件负向。
- ctest：`ma4b-anim-skeleton-selftest`（资产路径显式传源目录绝对路径）。

**踩坑（本轮）**
1. tinygltf 双 TU 宏视图不一致：IMPLEMENTATION 在 mesh_io.cpp（定义 `TINYGLTF_NO_STB_IMAGE_WRITE`），gltf_anim.cpp 未定义同宏 → inline 构造函数引用被禁的 `WriteImageData` → LNK2019。对齐宏后过；约定：**第二个 tinygltf 消费 TU 必须复刻 IMPLEMENTATION TU 的全部 NO_* 宏**。
2. FK 测试初版漏把 rest 平移填进 local——`local` 的语义是「采样输出」，glTF 流程里静态 TRS 已在轨道内；直接手构造 local 时必须以 restPose 打底。

## M-A4c：标准骨骼动画演示资产（2026-11-08 追加）

**资产**：`assets/models/rigged/` 三件套，来源 Khronos `glTF-Sample-Assets`（官方演示库，经 gh-proxy 镜像下载）：

| 资产 | 骨骼 | clips | 时长 | 用途 |
|---|---|---|---|---|
| `Fox.glb` | 26 | 3（Survey/Walk/Run） | 3.417/0.708/1.158 s | 多 clip 切换演示 |
| `CesiumMan.glb` | 22 | 1（行走） | 2.0 s | 业界经典单 clip 行走 |
| `RiggedFigure.glb` | 22 | 1 | 1.25 s | 标准蒙皮人体 |

**演示命令**：`anim-tools inspect <glb>`——打印骨架统计（bone/parent/restT 全表）+ clip 列表（名/时长/通道数）+ 末端骨骼 5 时刻 FK 世界位置。输出确定性（同资产两次运行逐位一致），作为 M-A5 GPU 视觉验收前的 CPU 端演示替身。

**加载器为真实资产补的两条口径**（都有手算/探针证据，非静默放宽）：
1. **动画 scale 通道**：值恒 1（|v-1|≤1e-4，CesiumMan/RiggedFigure 导出器空操作）→ 验证后丢弃；非恒 1 → 拒绝。BonePose 无 scale 口径不变。
2. **首 key 时间**：validateClip 的「first key must be 0」过严——glTF 只要求递增（CesiumMan translation 通道从 0.4167s 起步），域外 clamp 语义（M-A1 已实现）下首 key>0 自洽。改为拒绝负时间/非有限值。

**真 bug 修复**：collectBones 的 parentIndices 曾存 glTF 原始 node id，而骨架下标是 DFS 序——glTF 不保证 nodes 数组序=拓扑序（CesiumMan/RiggedFigure 父在子后），Fox 恰好两序一致所以 M-A4b 没暴露。修为 nodeId->boneIndex 映射。

**验收**：run.bat 三资产 inspect 关键行 findstr 锁定 ALL GREEN；cases.json ma-track 卡 +3 张 inspect（expectLog 锁 `bones N clips M`）；ctest ma* 6/6 无回归。

## M-A4d：骨骼动画动态演示页（2026-11-08 追加）

**演示命令**：`anim-tools fkdump <glb> <clipIndex> [frames=64]` —— 全骨骼逐帧 FK 世界坐标 TSV（拓扑头 + 每帧 `F t x0 y0 z0 ...`），确定性输出；`fksample` 同源（末端骨骼单点轨迹）。

**演示页**：`tools/anim_demo.py` 读取 dump 生成单文件 `example/ma-track/anim-demo.html`——四面板 canvas 循环播放 Fox Walk / Fox Run / CesiumMan Walk / RiggedFigure，蓝线段=骨骼、亮点=关节、白点=末端骨骼、淡色线=末端整周期轨迹；控件：播放/暂停、0.25~3× 速度、XY/ZY 视角切换（等比投影居中，切视角自动重算包围盒）。**页面零伪造**：数据 100% 来自 fkdump 真实输出，JS 只做投影与取帧；同 dump 两次生成 HTML 逐字节一致。

**验收**：run.bat 加 fkdump findstr 锁定行；cases.json ma-track 卡 +1（anim-demo，preview=演示页截图）；server --check OK。

## M-A5：蒙皮层 + viewer 模型展示（2026-11-08 追加）

**确定性自测**：`anim-tools selftest skinning [rigged.glb]`（缺省 `assets/models/rigged/CesiumMan.glb`）——刚体锚点不动点（head(-1,1)/hip 枢轴）、LBS 中点 (-0.5,1)、法线旋转归一、validateSkinBinding 五负向、资产端到端两条强判据：

| 判据 | 口径 |
|---|---|
| 网格撕裂检测 | 同一条边蒙皮后/绑定态长度比 < 2（全边扫描） |
| 主关节接近度 | 顶点与其主导权重关节的距离 < 1.2 m |

注：rest-identity（调色板≈identity）判据对 CesiumMan **无效**——bind=T-pose 而 rest=行走姿，IBM≠inverse(restGlobal) 是资产正常状态，勿再当断言用。

**viewer 模型展示**：`viewer --mode skin [--anim-time 秒] [--model rigged/X.glb]`——缺省 CesiumMan.glb，缺省距离 2.6；`--anim-time` 固定动画时刻（截图确定性），不传则实时累加。渲染 = CPU LBS（palette=model*IBM）逐帧写动态 VB，蒙皮输出按 t=0 包围盒中心平移居中（相机 target 硬编码原点的取景补偿）。

**回归**：cases.json ma-track 卡 `skinning` 用例（`{root}/assets/...` 路径——regression 工具用例 cwd=exe 目录，仓库根相对路径必挂）；run.bat 补 skinning 锁行。

**坑位（详 MILESTONES M-A5）**：JOINTS_0=skin.joints 槽位非 node id；node.matrix 刚性分解兜底；testGltfAssetLoad/testGltfDeterminism 加载失败必须早退（clips[0] 空 vector UB）。

### M-A5 补充：skindump + 蒙皮动画演示页（2026-11-09 追加）

- **`anim-tools skindump <rigged.glb> <clipIndex> [frames=48]`**：逐帧 CPU LBS 蒙皮顶点 TSV（确定性）。头部 `# skindump`（duration/verts/tris/frames）+ `IDX` 三角索引 + `DOM` 每顶点主导关节槽位（着色用）+ `F` 逐帧行（t + 每顶点 xyz，%.3f）。位置按 t=0 蒙皮输出包围盒中心平移——与 viewer `--mode skin` 同口径（bind 空间中心无效，见 M-A5.2 取景修正）。frames ∈ [2,512]。
- **`tools/skin_demo.py`** → `example/ma-track/skin-demo.html`（单文件 ~2.3MB，36 帧）：canvas 画 4672 三角——painter's algorithm 深度排序 + 面法线双面兰伯特 + 主导关节 hue 着色；相邻帧线性插值平滑播放。控件：播放/暂停、0.5~2× 速度、45°/正面/侧面预设、拖拽轨道、滚轮缩放、帧滑条；`#t=秒&pause=1` hash 锚点供确定性截图验证。页面只投影播放，不伪造任何一帧。
- **用例卡**：cases.json ma-track `skin-demo`（skindump 8 帧冒烟 + preview 挂 t=1.2s 页面截图）。
