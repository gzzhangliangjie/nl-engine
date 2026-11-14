#pragma once
// ---------------------------------------------------------------------------
// 资产文件名白名单（M-SC7 单源收编）。
//
// 历史：该逻辑在 renderer.cpp（--model/--scene 原版）为唯一实现；随后
// renderer/scene.cpp（initScene 兜底）、viewer/main.cpp（--scene 收集）、
// nav/SceneGeo.cpp（几何收集）各复制了一份语义——四处注释互相「指向不许漂移」
// 但本体各自生长（SceneGeo 版字符集更严：alnum+_-\.，其余三处 = 可打印 ASCII
// 0x21..0x7E）。M-SC7 脚本运行期 attach 是第 5 个消费点，复制不可再续——
// 全部收编到本 helper，单一语义 = renderer.cpp 原版（可打印 ASCII 0x21..0x7E，
// 不含空格；无路径分隔符；无 ".."；指定后缀）。SceneGeo 收编后放行字符集
// 略宽（如 '('），但该面只做文件名校验、路径穿越防护（分隔符 + ".."）语义
// 不变，viewer/renderer 兜底校验仍在同一条链上。
//
// 场景文件与脚本 meshFile 均来自用户，防路径穿越：basename + 指定后缀。
// ---------------------------------------------------------------------------

namespace nl::scene {

// 通用资产名白名单：可打印 ASCII（0x21..0x7E，不含空格）、无 '/' 与 '\\'、
// 无 ".."、以 ext 结尾且 ext 前至少 1 个字符。name 为空/NULL = false。
bool validAssetFileName(const char* name, const char* ext);

// .glb 特化（models/assets 引用面：--model / --scene meshFile / 脚本 attach）。
inline bool validGlbAssetName(const char* name) {
    return validAssetFileName(name, ".glb");
}

} // namespace nl::scene
