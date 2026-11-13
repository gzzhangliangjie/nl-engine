#pragma once

// ---- M-CRASH0：进程级崩溃捕获（关联系统，薄封装） ----
// 捕获未处理 SEH 异常 / std::terminate / 纯虚调用 / CRT 非法参数，
// 在 exe 同目录 crashdumps/ 下写 <exe>_<时间戳>_<异常码>.dmp + 同名 .json 元数据。
// 零第三方依赖（仅系统 dbghelp.dll）。需要时 WinDbg/VS 加载 dmp + 同版本 PDB 直接开。

namespace nl::crash {

// 安装进程级崩溃捕获。必须在 main() 第一行调用。幂等（仅首次生效）。
void install();

// 验收专用：故意触发一次空指针解引用崩溃（被 install() 的过滤器捕获并落 dump）。
// 仅在 --crash-test 等显式验收路径调用。
void trigger_test_crash();

} // namespace nl::crash
