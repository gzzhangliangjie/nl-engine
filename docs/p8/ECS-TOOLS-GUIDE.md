# nanite-scene 指南（P8 ECS 轨道 E1a 工具版交代）

> A 类并行轨道（`docs/PARALLEL-TRACKS.md` §7.1c / `docs/p8/P8-ECS-PLAN.md`）。
> 不进 `kStage`、不进渲染回归总闸；交代 = 展示台 tool 卡实跑 + 真实输出生成的证据图 + 本文归档。
> 范围：M-E0~E3 = ROADMAP「P8 E1」的 **E1a 拆分段**（纯逻辑内核）；E1b（接 viewer 渲染）为后挂段，届时另编号并触发渲染纪律。

## 用法

```
nanite-scene selftest --suite ecs-core|ecs-hier|ecs-system|ecs-serial [--verbose]
nanite-scene deserialise-check good|badjson|unknown
```

退出码：`0` 成功 / `1` 断言失败 / `2` 用法错误或显式拒绝。工具只链 `nl_core`（零图形 API 符号），构建：

```
scripts\build-m0.2.bat DX12    # vcvars + NMake（沙箱 reg 黑名单下的 INCLUDE/LIB 手工补丁）
```

## selftest 覆盖（全部确定性断言，两次运行逐位一致；--verbose 逐断言打印 ok 行）

| suite | 里程碑 | 覆盖 |
|---|---|---|
| `ecs-core` | M-E0 | 递增 ID（0 无效）/ 延迟增删 flush 前后存在性（Carrot 非对称口径：新建即时可见、删除 flush 后不可见）/ 名字读写 / Hidden 位标志过滤（隐藏≠删除）/ 重复 remove 幂等 / 延迟语义锚点（删实体下一 tick 生效） |
| `ecs-hier` | M-E1 | 三层父子链复合矩阵手算锚（A(1,0,0)·s2 ← B(0,1,0)·s3·Rz90 ← C(1,1,0)：C 世界矩阵 col0(0,6,0) col1(−6,0,0) col2(0,0,6) col3(−5,8,0)；局部点 (1,1,0)→世界 (−11,14,0)；getWorldPosition=局部原点=(−5,8,0)）/ reparent 保世界矩阵逐元素不变 + 新局部手算锚 pos(−3,4,0) scale(3) rot=Rz90 / 无 Transform 父直通（Carrot 断链口径）/ 删父递归删子 + 双结构清理 / duplicate 深拷（"(Copy)" 名、改副本原件不动、副本孙世界矩阵一致、父关系继承）/ 组件库（显式注册、按名创建、类型 ID 稳定、未知名 nullptr） |
| `ecs-system` | M-E2 | 签名匹配收编（子集判定 `(entitySig & systemSig) == systemSig`）/ 非匹配不收 / 组件挂上**即时**收编（Carrot 非对称：组件增删即时推送，实体才走 flush）/ 删组件自动出列 / 删实体 flush 后出列 / firstTick 恰一次 / tick 计数与 dt 传递 / duplicate 数据拷贝换 World |
| `ecs-serial` | M-E2 | **roundtrip 逐字节一致硬判据**（serialise→deserialise→serialise）+ ID/名字/层级/Hidden/组件数值（非平凡 quat float→double→float 逐位无损）/世界矩阵重建/nextID 接续不重用全抽查 + 五路负向显式拒绝（未知组件 / 未知父 / 未知系统 / 坏版本 / 重复 ID，err 非空不 assert） |

## deserialise-check 子命令（负向演示）

```
nanite-scene deserialise-check good      # 合法场景文件 -> exit 0
nanite-scene deserialise-check badjson   # 非法 JSON -> exit 2 + err 非空（显式拒绝，不静默不 assert）
nanite-scene deserialise-check unknown   # 未知组件名 -> exit 2 + err 指名
```

## 场景 JSON 格式（M-E2 拍板：单文件自定格式，字段口径借 Carrot）

```json
{
 "version": 1,
 "nextID": <u64>,
 "entities": [
  { "id": 1, "name": "...", "flags": 0, "parent": null,
    "components": { "Transform": { "position": [..], "scale": [..], "rotation": [x,y,z,w] } } }
 ],
 "systems": [ { "type": "CounterSystem", ... } ]
}
```

确定性口径：实体按 id 升序、组件键按名字典序（nlohmann std::map）、systems 按注册序 → 同一场景两次 serialise 逐字节一致（roundtrip 判据的地基）。反序列化三轮：壳 → 挂父/组件 → flush 后注册系统；未知组件/系统/父、重复 ID、坏版本全部显式拒绝。

## 复现

```
example\ecs-e1a\run.bat    # 四 suite + 负向 exit 2 校验 + 证据图重生成，ALL GREEN 才 exit 0
```

ctest（构建目录内）：`e0-ecs-core-selftest` / `e0-ecs-unknown-suite`（WILL_FAIL 负向）/
`e1-ecs-hier-selftest` / `e2-ecs-system-selftest` / `e2-ecs-serial-selftest` /
`e2-ecs-badjson-exit2` / `e2-ecs-unknown-comp-exit2`。

## 证据图

`python tools/make-ecs-evidence.py` 消费 `example/ecs-e1a/*.log`（工具真实输出重定向落盘）画
4 张 terminal 风格 PNG（Pillow，Consolas 字体，全部内容来自真实输出，无手绘示意）：
`usage.png` / `system.png` / `serial.png` / `negative-badjson.png`。**两次生成逐字节一致。**

## 踩坑存档（详版见 MILESTONES.md M-E0~E3 与当日记忆）

1. `unordered_map::contains` 是 C++20 → MSVC C2039（项目 C++17），一律 `find != end`。
2. 头文件引入 `unique_ptr` 别忘 `<memory>`；`#include` 永远在 namespace 外（bitset 进 namespace 内 = 标准库头全线爆炸）。
3. 「世界位置」语义：`getWorldPosition` = Carrot `computeFinalPosition` = **局部原点**的世界位置（W 平移列），与任意局部点变换（`mat4TransformPoint`）是两个语义，锚点分开写。
4. `duplicate(w).get()` 临时 unique_ptr 悬空 use-after-free —— 表现为 suite exit 127 无输出（stdout 缓冲随崩溃丢失）；所有权先落 named unique_ptr。
5. 组件推送与实体 flush 时机非对称：组件增删**即时**推送 onEntitiesUpdated，实体增删走 flush——断言时机写错会假 FAIL。
6. 工具输出字符串一律 ASCII（证据图 Consolas 无中文字形；注释保留中文）。

## M-E1b.0：viewer --scene 渲染集成（2026-11-09）

E1a 的纯逻辑内核接上渲染器。用户拍板：**方向灯数组** + **kStage 保持 S6.2 不 bump**（console 轨道先例）。

### 用法

```
build/DX12/viewer.exe --scene example/ecs-e1b/ecs-e1b.scene --no-hud \
    --screenshot <out.png> --frames 8 --size 960x540 --yaw 12 --pitch 9
example/ecs-e1b/run.bat          # 双后端复拍 + 机器断言，exit 0 = ok
python tools/regression.py --only ecs-e1b
```

场景文件是 `World::serialise` 的 JSON（version=1 / nextID / entities[id 升序] / systems），加一个场景级扩展键 `ambient`（World 格式不感知，viewer 读取，缺省 0.28）。组件库显式注册 `Transform` / `Model` / `Light`；`Model` 必须有 `Transform`，`mesh` 只接受 assets 白名单内的 ASCII `*.glb` 文件名；`Light` 不需要 Transform，方向在 viewer 侧归一化。

### 数据布局（新增资源，既有路径零改动）

| 槽 | 内容 |
|---|---|
| t5 `g_instanceData` | 每实例 5 个 float4：4 列世界矩阵（列主序）+ `(tint.rgb, 0)`，按 meshFile 分组连续排布 |
| t6 `g_lights` | `[0]=(lightCount, ambient, 1/ΣI, 0)`；之后每灯 2 个 float4：`{dir.xyz, intensity}`、`{color.rgb, 0}` |

`vsScene` 用 `SV_InstanceID`（含 `startInstanceLocation` 的全局槽号）取世界矩阵，`psScene` 循环灯数组累加 `abs(dot(n,dir))*color*intensity`，按 `1/ΣI` 归一化成加权平均避免过曝，ambient 兜底。灯数为 0 时退化为纯 ambient。

### 证据

- `example/ecs-e1b/shot-DX12.png` / `shot-VULKAN.png`（基线）+ 双后端 `.log`；归档图 `docs/screenshots/ecs-e1b-dx12.png` / `ecs-e1b-vulkan.png`。
- 肉眼核验：5 个 Suzanne（红/绿/蓝/黄/白）不同位置、朝向、缩放，顶面亮侧面暗的方向光层次。
- 双后端 `5 objects | 3 lights | batches=1 | triangles=3936 | deviceErrors=0`；`run.bat` exit 0；`--only ecs-e1b` 2/2 bytes；`--only s6.2` 10/10 bytes（kStage 未动、既有路径零回归）。
- 双后端互比 3px / maxDelta 2，超项目 near 阈值（≤2px 且 ≤1/255）→ 如实记 FAIL 档（GPU 取整噪声），各自对本后端基线是 bytes。

### 踩坑（详版见 MILESTONES.md M-E1b.0 段）

1. **组件类型 ID 全 0**（E1a 遗留）：`ComponentOf<T>::nextTypeID()` 的静态计数器在类模板成员函数里，每个 T 各一份 → `getComponent<Light>` cast 到 Model 内存 → 虚假灯数 + 段错误 139。修到非模板 `inline allocateComponentTypeID()`。**加新组件类型必须验 ID 隔离**。
2. BindingSet 早于实例/灯缓冲创建 → NVRHI 拒绝空 buffer 绑定；延后到第 5.5 步。
3. `instanceStart/instanceCount` 从没赋值 + 实例数据未按批连续排布 → `InstanceCount=0` 空画面。
4. 多灯强度直接相加过曝（ΣI≈4.7 全 clamp 1.0）→ 归一化；`1/ΣI` 放 CPU 端让 shader 只做乘法。
5. 联合包围球首球漏 ±r → 自动取景偏近。
6. 回归 cwd = exe.parent，场景相对路径失效 → `{root}` 占位符。
7. Windows 大小写不敏感：清理小写临时名误删大写基线；CRLF 文件文本插入要先归一化换行。

## M-E1b.1 — 规模锚 50 物体 × 8 方向灯（2026-11-09）

- 命令：`viewer.exe --scene {root}/example/ecs-e1b/ecs-e1b-scale.scene --no-hud --screenshot <out.png> --frames 8 --size 960x540 --yaw 12 --pitch 9 --dist 18`；一键复拍 `example/ecs-e1b/run-scale.bat`（双后端 + 机器断言，exit 0）。
- 场景：10×5 网格，4 种 mesh 按 `(col+row)%4` 分配 → 4 个 batch（13/13/12/12），58 实体（50 Model + 8 Light）；tint 走 HSV 全色相环，8 盏灯方向 / 色温 / 强度各异，ambient 0.22。
- 口径：`P8-ECS-PLAN.md` 目标「50 物体 + 8 灯规模」达成；三处参考确认无硬上限，故本轮不改架构，只扩场景数据。kStage 仍 S6.2。
- 双后端 `50 objects | 8 lights | batches=4 | triangles=540352 | deviceErrors=0`；`--only ecs-e1b` 4/4 bytes、`--only s6.2` 10/10 bytes；M-E1b.0 基线复拍 MD5 不变。
- 展示台：`example/cases.json` 的 `ecs-e1b` 卡新增 `scale` pair 用例（`{root}` 绝对路径 + `--dist 18`）；`--dist 18` 是为绕开自动取景被高密度 mesh 包围球撑远的问题，viewer 已有参数，不改取景公式。
- **实例化铁律**：禁止用 `startInstanceLocation` 让 `SV_InstanceID` 当全局槽号（DX12 不把它计入实例号）→ 每个 batch 各持自己的 t5 实例缓冲。详 `docs/SHADER-CONVENTIONS.md` §2.10。
- 双后端互比 6px / maxDelta 23 → 按阈值落 FAIL 档（5 个 ±1 取整 + 1 个环面边缘覆盖差），各自对本后端基线 bytes；如实留痕，不再折腾精度。
