# nanite-tools 使用说明与结果归档（每步交代）

> 工具版本里程碑的「可亲眼看到」= 本文的实跑输出归档；渲染版本 = `example/` 截图。
> 每个里程碑完成后，把当步实跑输出追加进对应小节，作为验收证据存档。
> 归档输出一律从已构建的 exe 现场跑出来贴入，不手写。

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

## M1.1 交代（待完成）

（M1.1 完成后追加：cluster 簇统计输出 + selftest 单元测试结果）
