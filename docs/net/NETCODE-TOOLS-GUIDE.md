# NETCODE-TOOLS-GUIDE — 网络栈工具归档（docs/net/，net 轨道）

> 工具 = `net-tools`（`build/{dx12,vulkan}/net-tools.exe`，纯 CPU 零图形 API，双构建目录同一二进制口径）。
> 退出码：**0 = PASS / 1 = 错误（建链失败等）/ 2 = 用法错误或超时**（对齐 nav-tools）。
> 展示台版本卡：`example/cases.json` `net-m0`（6 用例 `kind:"tool"`）；`example/net-m0/run.bat` 为 run.bat 交代。

## 1. M-NET0 — 传输层（2026-11-13，tag `net-m0`）

### 1.1 子命令

| 命令 | 形态 | 退出码 | 说明 |
|---|---|---|---|
| `echo` | **单进程回环**（同进程 server+client 双 host，单线程交替泵） | 0=OK / 1=init或beginConnect失败 / 2=超时 | `--port`(默认29191) `--reliable`(32) `--unreliable`(64) `--payload`(64) `--timeout-ms`(10000)；可靠+不可靠双通道全量回显断言，rtt 行为时变字段不作锚 |
| `echo-server <port>` | 双进程服务端 | 0=服务满 / 1=start失败 / 2=超时 | `--packets`(64) `--max-clients`(4) `--linger-ms`(0) `--timeout-ms`(30000)；linger>0 时服务满后继续存活 N ms（丢包 demo 收尾用） |
| `echo-client <host> <port>` | 双进程客户端 | 0=全回显 / 1=连接失败或断链 / 2=丢包或超时 | `--packets`(32) `--rate`(200) `--loss`(0) `--payload`(64) `--timeout-ms`(10000)；`--loss P` 模拟入站丢包（`mt19937` 固定种子 1234，确定性序列） |
| `selftest` | 机械断言 | 0=PASS / 1=FAIL | 16 可靠 + 32 不可靠小参数回环 + 全量回显断言 |
| `version` | 溯源锚 | 0 | 输出 `enet : 1.3.18`（ENET_VERSION 宏拼接，防库漂移） |

### 1.2 ctest（双后端各 5 条，`--repeat until-pass:3` 口径）

| 用例 | 断言 |
|---|---|
| `net-m0-echo` | `echo        : OK`（32/32 + 64/64 全量回显） |
| `net-m0-selftest` | `selftest net: PASS` |
| `net-m0-version` | `enet        : 1.3.18` |
| `net-m0-usage-negative` | 未知子命令 exit 2（`WILL_FAIL TRUE`） |
| `net-m0-connect-refused` | 死端口握手 exit 1（仅 `WILL_FAIL TRUE`；FAILED 文本锚由 run.bat findstr 兜底） |

> ⚠️ **WILL_FAIL + PASS_REGULAR_EXPRESSION 组合语义是「正则不得命中」**（ctest 实测反向），负向例只留 WILL_FAIL——口径对齐 anim-tools。

### 1.3 run.bat 段落（`example/net-m0/run.bat`，ALL GREEN 基线 2026-11-13）

1. version 锚 → 2. usage（exit 2）→ 3. 单进程回环（32/32 + 64/64 + `echo OK`）→ 4. selftest → 5. **双进程零丢失**（server 29192 + client 32 包 `sent=32 recv=32 loss=0.0%` + 轮询 server `served=32`）→ 6. **双进程 30% 丢包注入**（`--loss 30` 种子 1234 实测 recv=22 loss=31.2%，exit 2 为预期）→ 7. 死端口负向（exit 1 + `connect : FAILED`）。

### 1.4 踩坑留档（M-NET0 实测）

1. **ENet 服务器只在 `enet_host_service` 时处理握手包**：单进程回环里 `connect()` 阻塞等 CONNECT 期间同线程 server 没在泵 → 必超时。回环路径必须 `beginConnect()` + 双 host 交替泵（`connect()` 仍可用于双进程：对端在自己进程里泵）。
2. **`WILL_FAIL TRUE` 与 `PASS_REGULAR_EXPRESSION` 不可同用**：组合语义 = 正则不得命中（见 §1.2 注）。
3. **沙箱回收 `start` 派生子进程**：`start "title" /min` 开新窗的写法在沙箱会话必死（server.log 空、client 连不上）；`start /b ""` 直起 exe 子进程存活且继承重定向句柄。正常 cmd 环境两种都可用，run.bat 取 /b 兼容两者。
4. **run.bat ASCII only 重申**：rem 注释里的 em dash（—）在 GBK 控制台被 UTF-8 双字节拆碎，后续行解析错位（`'m' 不是内部或外部命令`），非 ASCII 一律用 ASCII 连字符。
5. **入站丢包无重发**：模拟丢包丢弃的回显没有第二次机会，server 必须撑到 client 先到 deadline 打出统计（`--linger-ms` 的由来）。

## 2. M-NET1（序列化层，bitsery）— 未开工

## 3. M-NET2（服务器权威最小闭环）— 未开工

## 4. M-NET3（打磨收官）— 未开工
