# nanite-tools 使用说明与结果归档（每步交代）

> **交代载体（2026-11-07 固化，规范见 `docs/ROADMAP.md`「里程碑交代规范」节）**：
> 首选展示台网页 `tools/showcase/start.bat` → http://127.0.0.1:8787/ —— s1-tools
> 版本卡上有 `kind:"tool"` 用例，点「运行工具」现场实跑、输出直接展开；
> 本文是同一批输出的**文本归档**（含当时实跑记录），两者同源、退出码语义一致
> （0 成功 / 1 用法或运行错误 / 2 已声明未实现）。渲染版本 = `example/` 截图基线。

## 定位

S1~S3 的离线网格处理管线（切簇 / 重切 / 误差 DAG），为 Nanite 式虚拟几何准备原料。
只依赖 `nl_core`（日志 + `core/mesh_io` 公共 glTF 读取层），零图形 API 符号；
与 viewer 的架构纪律一致（图形 API 只允许活在 `renderer/`）。

## 构建

```bat
scripts\build-m0.2.bat DX12      rem 或 VULKAN；产物 build\<backend>\nanite-tools.exe
```

## 子命令与退出码

| 子命令 | 用法 | 里程碑 | 退出码约定 |
|---|---|---|---|
| `stat` | `nanite-tools stat <model.gltf\|.glb>` | M1.0 | 成功 0；文件/解析失败 1 |
| `cluster` | `nanite-tools cluster <model>` | M1.1 | 未实现=2 |
| `simplify` | `nanite-tools simplify <model>` | S2 | 未实现=2 |
| `dag` | `nanite-tools dag <model>` | S3 | 未实现=2 |
| （无参数 / 未知子命令） | — | — | 打印 usage，退出 1 |

约定：`0` 成功、`1` 用法错误或运行失败、`2` 子命令已声明但未实现（防静默假成功）。

## M1.0 交代（tag `s1-tools`，commit `58b0270`）

### usage（无参数实跑）

```
nanite-tools - nl-engine offline mesh pipeline (S1)
usage:
  nanite-tools stat <model.gltf|.glb>       vertex/tri/submesh summary
  nanite-tools cluster <model> [options]    meshlet build (M1.1, not yet)
  nanite-tools simplify <model> [options]   cluster re-cut (S2, not yet)
  nanite-tools dag <model> [options]        error DAG (S3, not yet)
```

### stat 实跑输出（DX12 构建，2026-11-07）

```
file      : assets/models/Suzanne.glb
submeshes : 1
vertices  : 11808
triangles : 3936
aabb      : min(-1.3369, -0.9746, -0.8008) max(1.3369, 0.9502, 0.8257)
aabb size : 2.6738 x 1.9248 x 1.6265
  [0] Suzanne#0                vertices=11808   triangles=3936    tex=yes

file      : assets/models/TorusKnotDense.glb
submeshes : 1
vertices  : 103113
triangles : 204800
aabb      : min(-1.2684, -0.5680, -0.8710) max(1.3680, 0.5680, 1.3422)
aabb size : 2.6364 x 1.1360 x 2.2132
  [0] mesh#0#0                 vertices=103113  triangles=204800  tex=yes

file      : assets/models/IcoSphereDense.glb
submeshes : 1
vertices  : 163842
triangles : 327680
aabb      : min(-1.0000, -1.0000, -1.0000) max(1.0000, 1.0000, 1.0000)
aabb size : 2.0000 x 2.0000 x 2.0000
  [0] mesh#0#0                 vertices=163842  triangles=327680  tex=yes
```

**核对**：三个资产与 M0.7 已知口径全对上（Suzanne 3,936 tri；纽结 204,800；球 327,680）；
球 AABB ±1.0 严丝合缝 = 节点变换烘焙正确。ctest `s1-tools-stat` 用正则锁死
Suzanne `triangles : 3936`，资产口径漂移会当场爆。

### 负向路径实跑

| 命令 | 输出 | 退出码 |
|---|---|---|
| `nanite-tools bogus` | `unknown subcommand: bogus` + usage | 1 |
| `nanite-tools cluster` | `cluster: not implemented yet (S1 M1.1+ / S2 / S3)` | 2 |
| `nanite-tools stat nofile.glb` | `stat: glTF load (nofile.glb) failed: …File open error` | 1 |

### 渲染侧零影响证明

scene.cpp 改走 `nl::core::loadGltfScene` 后：`python tools/regression.py` **58/58 ALL
GREEN**（全部逐字节命中）、viewer --selftest 15/15（DX12 + Vulkan 双后端）、
`server.py --check` 退出 0。M1.0 是纯工具版本，kStage 保持 M0.9、example/ 不动。

## M1.1 meshlet 贪心切簇（tag `s1-cluster`，2026-11-07）

> 算法：meshoptimizer clusterizer 裁剪版（拓扑优先级 + 空间评分贪心吸收），
> 128 tri / 64 vert 双上限；Ritter 包围球 + 法线锥 + 世界空间叶子误差（簇内最长边长）。
> 关键取舍：**几何邻接按位置 weld 建**——Suzanne 每 tri 拆 3 个独立顶点（索引零共享），
> 按索引建邻接会退化成每 tri 一簇；GPU 条目上限按原始索引口径（保留 UV 缝属性）。

### 子命令变化

- `cluster <model> [-v]`：已实现（M1.1）。`-v` 追加前 5 簇明细。
- `selftest cluster`：新增，确定性合成网格机械单测。
- `simplify` 仍退出 2（S2），`dag` 仍退出 2（S3）。

### selftest cluster 实跑（三确定性网格 + 负向输入）

```
[connected 64x64 grid (7938 tris)]
  ok   coverage 100%
  ok   cluster limits
  ok   bounds sane
[2 disconnected 32x32 blocks]
  ok   coverage 100%
  ok   cluster limits
  ok   bounds sane
[single quad]
  ok   coverage 100%
  ok   cluster limits
  ok   bounds sane
[input validation]
  ok   out-of-range index rejected
  ok   index count % 3 != 0 rejected
cluster selftest: PASS
exit=0
```

### cluster 实跑（三资产）

```
$ nanite-tools cluster assets/models/Suzanne.glb -v
file       : assets/models/Suzanne.glb
submesh    : Suzanne#0
input      : 11808 verts / 3936 tris
meshlets   : 237
tri/cluster: avg 16.6  max 21  (limit 128)
vert/cluster: avg 49.8  max 63  (limit 64)
coverage   : 3936/3936 = 100.00%
error      : min 0.0308  avg 0.1167  max 0.3198  (world space, max edge)
cone       : avg cutoff 0.858  min 0.286  (degenerate: 1)
limits     : PASS
first 5 meshlets:
  [0] tri=21  vert=63  sphere r=0.5900 cutoff=0.684 err=0.1551
  [1] tri=21  vert=63  sphere r=0.4188 cutoff=0.798 err=0.1273
  [2] tri=21  vert=63  sphere r=0.2887 cutoff=0.875 err=0.1005
  [3] tri=21  vert=63  sphere r=0.2734 cutoff=0.874 err=0.0683
  [4] tri=21  vert=63  sphere r=0.4973 cutoff=0.842 err=0.1113
exit=0

$ nanite-tools cluster assets/models/IcoSphereDense.glb
input      : 163842 verts / 327680 tris
meshlets   : 4929
tri/cluster: avg 66.5  max 102  (limit 128)
vert/cluster: avg 45.3  max 64  (limit 64)
coverage   : 327680/327680 = 100.00%
error      : min 0.0098  avg 0.0101  max 0.0103
cone       : avg cutoff 0.999  min 0.996  (degenerate: 0)
limits     : PASS
exit=0

$ nanite-tools cluster assets/models/TorusKnotDense.glb
input      : 103113 verts / 204800 tris
meshlets   : 3094
tri/cluster: avg 66.2  max 98  (limit 128)
vert/cluster: avg 48.1  max 64  (limit 64)
coverage   : 204800/204800 = 100.00%
error      : min 0.0115  avg 0.0224  max 0.0355
cone       : avg cutoff 0.980  min 0.917  (degenerate: 0)
limits     : PASS
exit=0
```

**判读**：高模（索引共享充分）簇均值 ~66 tri / 48 vert、贴近 64 vert 上限，符合贪心预期；
Suzanne 簇小（avg 16.6 tri）不是 bug——每 tri 拆 3 独立顶点，64 条目上限 ~21 tri 就打满，
与 bevy 对未 weld 网格的行为一致。确定性验证：同 exe 连跑 4 次输出逐字相同。
算法数据结构即 M1.2 `.nmesh` 的序列化源（vertices/triangles 紧凑数组原样落盘）。

### 负向路径实跑（M1.1 增量）

| 命令 | 输出 | 退出码 |
|---|---|---|
| `nanite-tools cluster __missing__.glb` | `cluster: glTF load (...) failed: …File open error` | 1 |
| `nanite-tools simplify` | `simplify: not implemented yet (S2 / S3)` | 2 |
| `nanite-tools selftest bogus` | `selftest: unknown suite (available: cluster)` | 1 |

### 渲染侧零影响证明

viewer 一行未动：`python tools/regression.py` **70/70 ALL GREEN**（历史 64 条基线逐字节
不动 + 新增 s1-cluster 工具用例 3×2）、viewer --selftest 15/15 双后端、ctest 4/4
（新增 s1-cluster-selftest / s1-cluster-suzanne 正则锁定）。
