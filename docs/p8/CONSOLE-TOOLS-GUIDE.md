# nanite-console 使用说明与结果归档（P8 M-C3 交代）

> **交代载体**：展示台网页 `tools/showcase/start.bat` → http://127.0.0.1:8787/ ——
> `console-mc3` 版本卡上有 `kind:"tool"` 五用例现场实跑（usage / 正向 / 负向×2 / selftest）；
> 本文是同一批输出的文本归档。交互侧（ImGui 前端）证据 =
> `docs/screenshots/mc2-registry-dx12.png` / `mc2-fov100-dx12.png` / `mc2-registry-vulkan.png`。

## 定位

`core/console` 开发者控制台纯逻辑内核的**独立验证载体**（M-C0~C3 产物）：
不链图形后端、零 ImGui 依赖，同一个 exe 承载两条路径——

- `selftest`：49 条确定性单测（M-C0 42 + M-C2 内建 7），ctest 锁死；
- `exec`：M-C3 展示台用例的真实执行体（demo 注册表 = installBuiltins + demo.* 四类型 CVar + echo/add 命令）。

viewer 里的运行时形态（`` ` `` 呼出、`cam.fov`/`hud.show` 真参数回调、panelShow 解耦）
是同一份 `Console` 单例的第二个消费者，交互证据见上表截图。

## 退出码语义（与 nanite-tools 不同，如实分档）

`nanite-console` 的退出码 = `ExecCode` 分档，**不是** nanite-tools 的 0/1/2 约定：

| 退出码 | ExecCode | 场景 |
|---|---|---|
| 0 | Ok | 全部命令成功；selftest 全绿 |
| 1 | UnknownCommand | 注册表查无此名 |
| 2 | BadRequest | CLI 层缺参（`exec` 后没给命令行；无参数启动） |
| 3 | CommandFailed | 命令回调返回 false（add 非数字 / cvar.set 坏值保旧） |

多条命令时取 worst（`exec "ok" "bad"` → 3）。展示台 `expectExit` 逐用例实锁。

## 用法

```
nanite-console selftest                  # 49 条确定性单测，exit 0 即 PASS
nanite-console exec "<line>" [...]       # 多条命令按序进 demo 注册表
nanite-console                           # 无参 = usage，exit 2
```

demo 注册表：`demo.enabled`(bool) / `demo.count`(int) / `demo.fov`(float) /
`demo.name`(string) + `echo` / `add` 命令 + M-C2 内建 `help` / `cvar.list` / `cvar.set`。

## 展示台五用例实跑归档（2026-11-09，DX12/Vulkan 同一 exe）

### 1. usage（无参数）→ exit 2

```
usage:
  nanite-console selftest              deterministic unit tests
  nanite-console exec "<line>" [...]   run lines on demo registry
```

### 2. selftest → exit 0

```
  ok   builtins: cvar.set missing value -> usage fail
  ok   builtins: double-install no dup registrations
console selftest: PASS
```

（共 49 条 ok，首尾摘录；全量见 selftest 源码 `nanite-console/main.cpp` 八节。）

### 3. exec 正向：echo + cvar.list + cvar.set → exit 0

```
[0] echo: hello console
[0] cvars:
  demo.count = 3  (int)  demo int cvar
  demo.enabled = true  (bool)  demo bool cvar
  demo.fov = 60  (float)  demo fov cvar
  demo.name = nl-engine  (string)  demo string cvar
[0] demo.fov = 90
```

`cvar.set demo.fov 90` 后 `cvar.list` 再查即 `demo.fov = 90`（值真写进注册表）。

### 4. 负向：未知命令 → exit 1

```
[1] unknown command: nonsense.cmd
```

### 5. 负向：cvar.set 坏值保旧 → exit 3

```
[3] invalid value for demo.fov: expected float, got 'abc'
```

旧值不动（`demo.fov` 保持 60）。BadRequest(2) 留给 CLI 层缺参，
赋值失败归 CommandFailed(3)——分档口径见上表。

## 踩坑与口径（收口留存）

1. **退出码双约定**：展示台全局约定 0/1/2 来自 nanite-tools；console 的
   ExecCode 四档是本轨拍板的独立语义（`P8-CONSOLE-PLAN.md` M-C0 节）。
   两套并存，用例卡 note 里写明，展示台 `expectExit` 如实分档不为迁就全局约定改语义。
2. **selftest 退出码验证勿用管道**：`selftest | tail; echo $?` 拿到的是 tail 的
   退出码（本项目 M6.0 已留痕的环境级坑），必须无管道直跑。
3. 证据图纪律：截图一律落 `docs/screenshots/`（可提交位置），`build/out/`
   只作中间产物——M-C2 曾因图拍在 build/out 被点名「没有交代的图片」。
