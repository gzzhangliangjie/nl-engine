// plugin.cpp — 编辑器插件加载器（进程内注册 + 内建样例 + M-E5 plugins/ DLL 动态扫描）
#include "plugin.hpp"
#include <imgui.h>
#include <windows.h>
#include <cstdio>
#include <string_view>

namespace nl::editor::ui {

// 复用编辑器宿主侧的类型别名（位于兄弟命名空间 nl::editor::host）
using nl::editor::host::RpcHost;
using nl::editor::host::RpcValue;
using nl::editor::host::RpcError;
using nl::editor::host::RpcResult;

// M-E6：插件在 ImGui 渲染线程上直接发 RPC。Rpchost 默认 5s 阻塞超时意味着
// 「引擎连上但不回包」时整个编辑器主循环会卡 5s（多视口下每帧都要刷多个插件，
// 卡顿被放大成视口假死）。插件侧统一收敛到 500ms 快速失败——本机 loopback
// 往返远小于此，且掉线时 UI 仍能保持帧率。
static RpcResult callQuick(RpcHost* host, std::string_view method, const RpcValue& p) {
    return host->call(method, p, 500);
}

// M-E3：相机工具插件——「编辑器 UI → RPC call → 引擎相机状态变化 → 嵌入画面动」
// 的最小可见闭环。操作后回读 camera.getState 对齐 UI（引擎侧 clamp，回读为准）。
class CameraPlugin : public Plugin {
public:
    const char* name() const override { return "Camera"; }
    void draw(RpcHost* host) override {
        if (!host) { ImGui::TextDisabled("(no rpc host)"); return; }

        // ---- 状态区（camera.getState 回读）----
        if (ImGui::Button("Refresh")) refresh(host);
        ImGui::SameLine();
        if (have_) {
            ImGui::Text("fov %.1f  dist %.2f  yaw %.1f  pitch %.1f", fov_, dist_, yaw_, pitch_);
        } else {
            ImGui::TextDisabled("state: not fetched (engine offline or call failed)");
        }

        // ---- 控制区 ----
        float fv = fov_;
        if (ImGui::SliderFloat("FOV", &fv, 20.0f, 120.0f, "%.1f deg")) {
            if (callField(host, "camera.setFov", "fov", fv, fov_)) refresh(host);
        }
        float dv = dist_;
        if (ImGui::SliderFloat("Distance", &dv, 0.45f, 20.0f, "%.2f")) {
            if (callField(host, "camera.setDistance", "dist", dv, dist_)) refresh(host);
        }
        if (ImGui::Button("Orbit L")) { orbitDeg(host, -15.0f, 0.0f); }
        ImGui::SameLine();
        if (ImGui::Button("Orbit R")) { orbitDeg(host, 15.0f, 0.0f); }
        ImGui::SameLine();
        if (ImGui::Button("Orbit Up")) { orbitDeg(host, 0.0f, -10.0f); }
        ImGui::SameLine();
        if (ImGui::Button("Orbit Dn")) { orbitDeg(host, 0.0f, 10.0f); }
        if (ImGui::Button("Reset")) {
            auto r = callQuick(host, "camera.reset", RpcValue());
            logErr(r, "camera.reset");
            refresh(host);
        }
        ImGui::Separator();
        ImGui::TextDisabled("orbit: L/R +-15deg yaw, Up/Dn -+10deg pitch (deg->px 换算在编辑器侧)");
    }

private:
    // params 统一命名 Map（RPC-SPEC）；single-field 快捷封装
    static bool callField(RpcHost* host, const char* method, const char* key,
                          double val, float& outActual) {
        msgpack::Map m; m[key] = val;
        auto r = callQuick(host, method, RpcValue(std::move(m)));
        if (std::holds_alternative<RpcValue>(r)) {
            const auto& v = std::get<RpcValue>(r);
            if (v.isDouble()) { outActual = (float)v.asDouble(); return true; }
            if (v.isInt())    { outActual = (float)v.asInt(); return true; }
            return true; // ack 类型不符也当成功（refresh 会校正）
        }
        const auto& e = std::get<RpcError>(r);
        ImGui::Text("error %d: %s", e.code, e.message.c_str());
        return false;
    }

    // 引擎 camera.orbit 收像素增量（kOrbitGain=0.007 rad/px）；插件按度数语义，
    // deg -> px 换算在编辑器侧：px = deg / (0.007 rad/px * 57.2958 deg/rad) ≈ deg * 2.4913
    void orbitDeg(RpcHost* host, double degX, double degY) {
        constexpr double kPxPerDeg = 1.0 / (0.007 * 57.2958);   // ≈ 2.4913
        msgpack::Map m;
        m["dx"] = degX * kPxPerDeg;
        m["dy"] = degY * kPxPerDeg;
        auto r = callQuick(host, "camera.orbit", RpcValue(std::move(m)));
        logErr(r, "camera.orbit");
        refresh(host);
    }

    void refresh(RpcHost* host) {
        auto r = callQuick(host, "camera.getState", RpcValue());
        if (!std::holds_alternative<RpcValue>(r)) {
            logErr(r, "camera.getState"); have_ = false; return;
        }
        const auto& v = std::get<RpcValue>(r);
        if (!v.isMap()) { have_ = false; return; }
        auto get = [&](const char* k, double def) {
            auto it = v.asMap().find(k);
            return (it != v.asMap().end() && it->second.isDouble()) ? it->second.asDouble() : def;
        };
        fov_   = (float)get("fov", 60.0);
        dist_  = (float)get("dist", 4.2);
        yaw_   = (float)get("yaw", 0.0);
        pitch_ = (float)get("pitch", 35.0);
        have_  = true;
    }

    static void logErr(const RpcResult& r, const char* what) {
        if (!std::holds_alternative<RpcError>(r)) return;
        const auto& e = std::get<RpcError>(r);
        ImGui::Text("[%s] error %d: %s", what, e.code, e.message.c_str());
    }

    bool   have_ = false;
    float  fov_ = 60.0f, dist_ = 4.2f, yaw_ = 0.0f, pitch_ = 35.0f;
};

// M-E4d：场景树插件——scene.getState 拉实例表（mesh/pos/tint/rough/metal），
// 点击行调 scene.focus 让引擎 orbit 相机对准该对象。model 模式引擎返回
// mode="model" 空表，插件显示占位说明。
class SceneOutlinerPlugin : public Plugin {
public:
    const char* name() const override { return "Scene Outliner"; }
    void draw(RpcHost* host) override {
        if (!host) { ImGui::TextDisabled("(no rpc host)"); return; }
        // M-E4d：连接后首帧自动拉取（engine 掉线重连后 have_ 已被清，也会自动再拉）
        if (!have_ && host->isConnected()) refresh(host);
        if (ImGui::Button("Refresh")) refresh(host);
        ImGui::SameLine();
        if (!have_) {
            ImGui::TextDisabled("state: not fetched");
            return;
        }
        ImGui::Text("mode: %s  objects: %d", mode_.c_str(), (int)rows_.size());
        if (mode_ == "model") {
            ImGui::TextDisabled("(model mode: 单模型无实例表；--mode scene 可看实例树)");
            return;
        }
        if (rows_.empty()) return;
        // 实例表：行首 # + mesh 名；点击行 = scene.focus（引擎把 orbit 相机对准该对象）
        ImGui::BeginChild("outliner_rows", ImVec2(0, 160), ImGuiChildFlags_Borders);
        ImGui::Columns(4, nullptr, false);
        ImGui::SetColumnWidth(0, 28.0f);
        ImGui::SetColumnWidth(1, 110.0f);
        ImGui::SetColumnWidth(2, 96.0f);
        for (int i = 0; i < (int)rows_.size(); ++i) {
            const auto& r = rows_[i];
            char label[24]; snprintf(label, sizeof label, "%d", i);
            if (ImGui::Selectable(label, focusedIdx_ == i, ImGuiSelectableFlags_SpanAllColumns))
                focus(host, i);
            ImGui::NextColumn();
            ImGui::TextUnformatted(r.mesh.c_str());          ImGui::NextColumn();
            ImGui::Text("(%.1f, %.1f, %.1f)", r.x, r.y, r.z); ImGui::NextColumn();
            ImGui::Text("r%.2f m%.2f", r.rough, r.metal);     ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::EndChild();
        if (!focused_.empty()) ImGui::Text("focused: %s", focused_.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("click a row -> scene.focus (orbit camera aims at object)");
    }

private:
    struct Row { std::string mesh; float x = 0, y = 0, z = 0, rough = 1, metal = 0; };

    void refresh(RpcHost* host) {
        auto r = callQuick(host, "scene.getState", RpcValue());
        if (!std::holds_alternative<RpcValue>(r)) { logErr(r, "scene.getState"); have_ = false; return; }
        const auto& v = std::get<RpcValue>(r);
        if (!v.isMap()) { have_ = false; return; }
        const auto& m = v.asMap();
        auto itMode = m.find("mode");
        mode_ = (itMode != m.end() && itMode->second.isStr()) ? itMode->second.asStr() : "?";
        rows_.clear();
        auto itObjs = m.find("objects");
        if (itObjs != m.end() && itObjs->second.isArray()) {
            for (const auto& o : itObjs->second.asArray()) {
                if (!o.isMap()) continue;
                const auto& om = o.asMap();
                Row row;
                auto itMesh = om.find("mesh");
                if (itMesh != om.end() && itMesh->second.isStr()) row.mesh = itMesh->second.asStr();
                auto itPos = om.find("pos");
                if (itPos != om.end() && itPos->second.isArray() && itPos->second.asArray().size() == 3) {
                    const auto& pa = itPos->second.asArray();
                    if (pa[0].isDouble()) row.x = (float)pa[0].asDouble();
                    if (pa[1].isDouble()) row.y = (float)pa[1].asDouble();
                    if (pa[2].isDouble()) row.z = (float)pa[2].asDouble();
                }
                auto getF = [&](const char* k) {
                    auto it = om.find(k);
                    return (it != om.end() && it->second.isDouble()) ? (float)it->second.asDouble() : 0.0f;
                };
                row.rough = getF("rough");
                row.metal = getF("metal");
                rows_.push_back(std::move(row));
            }
        }
        have_ = true;
    }

    void focus(RpcHost* host, int i) {
        msgpack::Map p; p["i"] = (std::int64_t)i;
        auto r = callQuick(host, "scene.focus", RpcValue(std::move(p)));
        if (std::holds_alternative<RpcValue>(r)) {
            const auto& v = std::get<RpcValue>(r);
            if (v.isMap()) {
                auto g = [&](const char* k) {
                    auto it = v.asMap().find(k);
                    return (it != v.asMap().end() && it->second.isDouble()) ? it->second.asDouble() : 0.0;
                };
                char buf[96];
                snprintf(buf, sizeof buf, "#%d (yaw %.1f pitch %.1f dist %.2f)", i, g("yaw"), g("pitch"), g("dist"));
                focused_ = buf;
                focusedIdx_ = i;
                return;
            }
        }
        const auto& e = std::get<RpcError>(r);
        focusedIdx_ = -1;
        ImGui::Text("[scene.focus] error %d: %s", e.code, e.message.c_str());
    }

    static void logErr(const RpcResult& r, const char* what) {
        if (!std::holds_alternative<RpcError>(r)) return;
        const auto& e = std::get<RpcError>(r);
        ImGui::Text("[%s] error %d: %s", what, e.code, e.message.c_str());
    }

    bool have_ = false;
    std::string mode_ = "?";
    std::vector<Row> rows_;
    std::string focused_;
    int focusedIdx_ = -1;
};

// M-E5：材质检查插件——scene.getMaterials 读实例材质字段（scene 模式）或
// .nmesh NSUB 子网格/材质表（model 模式）；scene.setMaterial 改 tint/rough/metal
// （viewer 每帧原地刷新，改动下一帧可见）。
class MaterialInspectorPlugin : public Plugin {
public:
    const char* name() const override { return "Material Inspector"; }

private:
    struct Row  { std::string mesh; float tint[3] = {1, 1, 1}; float rough = 1, metal = 0; };
    struct Sub  { long long matId = 0, clusterFirst = 0, clusterCount = 0; };

    void draw(RpcHost* host) override {
        if (!host) { ImGui::TextDisabled("(no rpc host)"); return; }
        // 连接后首帧自动拉取（掉线重连后 have_ 清空也会自动再拉）
        if (!have_ && host->isConnected()) refresh(host);
        if (ImGui::Button("Refresh")) refresh(host);
        ImGui::SameLine();
        if (!have_) { ImGui::TextDisabled("state: not fetched"); return; }

        if (mode_ == "scene") {
            drawScene(host);
        } else {
            drawModel();
        }
    }

    // ---- scene 模式：实例材质表 + 选中行编辑 ----
    void drawScene(RpcHost* host) {
        ImGui::Text("mode: scene  objects: %d", (int)rows_.size());
        if (rows_.empty()) return;
        ImGui::BeginChild("mat_rows", ImVec2(0, 150), ImGuiChildFlags_Borders);
        ImGui::Columns(5, nullptr, false);
        ImGui::SetColumnWidth(0, 24.0f);
        ImGui::SetColumnWidth(1, 100.0f);
        ImGui::SetColumnWidth(2, 64.0f);
        ImGui::SetColumnWidth(3, 64.0f);
        for (int i = 0; i < (int)rows_.size(); ++i) {
            const auto& r = rows_[i];
            char label[16]; snprintf(label, sizeof label, "%d", i);
            if (ImGui::Selectable(label, selected_ == i, ImGuiSelectableFlags_SpanAllColumns))
                selected_ = i;
            ImGui::NextColumn();
            ImGui::TextUnformatted(r.mesh.c_str()); ImGui::NextColumn();
            ImGui::Text("r%.2f", r.rough);           ImGui::NextColumn();
            ImGui::Text("m%.2f", r.metal);           ImGui::NextColumn();
            ImGui::ColorButton("tint", ImVec4(r.tint[0], r.tint[1], r.tint[2], 1.0f),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::EndChild();

        // ---- 选中实例的材质编辑（tint/rough/metal → scene.setMaterial）----
        if (selected_ < 0 || selected_ >= (int)rows_.size()) return;
        Row& r = rows_[selected_];
        ImGui::Separator();
        ImGui::Text("editing #%d (%s)", selected_, r.mesh.c_str());
        bool dirty = false;
        if (ImGui::ColorEdit3("tint", r.tint, ImGuiColorEditFlags_NoInputs))
            dirty = true;
        if (ImGui::SliderFloat("rough", &r.rough, 0.0f, 1.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("metal", &r.metal, 0.0f, 1.0f, "%.2f")) dirty = true;
        if (dirty) setMaterial(host, selected_, r);
        ImGui::TextDisabled("edits call scene.setMaterial; viewer refreshes next frame");
    }

    // ---- model 模式：.nmesh NSUB 子网格/材质表（材质 id = 子网格序号）----
    void drawModel() {
        ImGui::Text("mode: model  file: %s", modelFile_.c_str());
        if (nmErr_ >= 0) {
            if (nmErr_ == 0) ImGui::TextDisabled("(not a .nmesh asset — no NSUB info)");
            else             ImGui::TextDisabled("nmesh read failed: %s", nmErrText_.c_str());
            return;
        }
        ImGui::Text("mesh: %s  meshlets: %lld  submeshes: %lld",
                    nmName_.c_str(), (long long)nmMeshlets_, (long long)nmSubCount_);
        if (nmSubs_.empty()) {
            ImGui::TextDisabled("(single submesh — materialId = 0)");
            return;
        }
        ImGui::BeginChild("nsub_rows", ImVec2(0, 110), ImGuiChildFlags_Borders);
        ImGui::Columns(3, nullptr, false);
        ImGui::SetColumnWidth(0, 90.0f);
        ImGui::SetColumnWidth(1, 90.0f);
        for (const auto& s : nmSubs_) {
            ImGui::Text("material %lld", (long long)s.matId); ImGui::NextColumn();
            ImGui::Text("clusters %lld..", (long long)s.clusterFirst); ImGui::NextColumn();
            ImGui::Text("count %lld", (long long)s.clusterCount); ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::EndChild();
        ImGui::TextDisabled("(model mode: 无实例材质字段，setMaterial 仅 scene 模式)");
    }

    void refresh(RpcHost* host) {
        auto r = callQuick(host, "scene.getMaterials", RpcValue());
        if (!std::holds_alternative<RpcValue>(r)) { logErr(r, "scene.getMaterials"); have_ = false; return; }
        const auto& v = std::get<RpcValue>(r);
        if (!v.isMap()) { have_ = false; return; }
        const auto& m = v.asMap();
        auto getStr = [&](const char* k) {
            auto it = m.find(k);
            return (it != m.end() && it->second.isStr()) ? it->second.asStr() : std::string();
        };
        mode_ = getStr("mode");
        rows_.clear();
        auto itObjs = m.find("objects");
        if (itObjs != m.end() && itObjs->second.isArray()) {
            for (const auto& o : itObjs->second.asArray()) {
                if (!o.isMap()) continue;
                const auto& om = o.asMap();
                Row row;
                auto itMesh = om.find("mesh");
                if (itMesh != om.end() && itMesh->second.isStr()) row.mesh = itMesh->second.asStr();
                auto itTint = om.find("tint");
                if (itTint != om.end() && itTint->second.isArray()) {
                    const auto& ta = itTint->second.asArray();
                    for (int c = 0; c < 3 && c < (int)ta.size(); ++c)
                        if (ta[c].isDouble()) row.tint[c] = (float)ta[c].asDouble();
                }
                auto getF = [&](const char* k) {
                    auto it = om.find(k);
                    return (it != om.end() && it->second.isDouble()) ? (float)it->second.asDouble() : 0.0f;
                };
                row.rough = getF("rough");
                row.metal = getF("metal");
                rows_.push_back(std::move(row));
            }
        }
        // model 模式：.nmesh 子网格/材质表
        modelFile_ = getStr("file");
        nmErr_ = -1; nmSubs_.clear(); nmName_.clear(); nmMeshlets_ = 0; nmSubCount_ = 0;
        auto itNm = m.find("nmesh");
        if (itNm != m.end() && itNm->second.isMap()) {
            const auto& nm = itNm->second.asMap();
            auto nmStr = [&](const char* k) {
                auto it = nm.find(k);
                return (it != nm.end() && it->second.isStr()) ? it->second.asStr() : std::string();
            };
            auto nmI = [&](const char* k) -> long long {
                auto it = nm.find(k);
                if (it != nm.end() && it->second.isInt()) return it->second.asInt();
                if (it != nm.end() && it->second.isDouble()) return (long long)it->second.asDouble();
                return 0;
            };
            auto itErr = nm.find("error");
            if (itErr != nm.end() && itErr->second.isStr()) {
                nmErr_ = 1; nmErrText_ = itErr->second.asStr();
            } else {
                nmName_ = nmStr("name");
                nmMeshlets_ = nmI("meshlets");
                nmSubCount_ = nmI("submeshCount");
                auto itSubs = nm.find("submeshes");
                if (itSubs != m.end() && itSubs->second.isArray()) {
                    for (const auto& s : itSubs->second.asArray()) {
                        if (!s.isMap()) continue;
                        const auto& sm = s.asMap();
                        Sub g;
                        auto gi = [&](const char* k) -> long long {
                            auto it = sm.find(k);
                            return (it != sm.end() && it->second.isInt()) ? it->second.asInt() : 0;
                        };
                        g.matId = gi("materialId");
                        g.clusterFirst = gi("clusterFirst");
                        g.clusterCount = gi("clusterCount");
                        nmSubs_.push_back(g);
                    }
                }
            }
        } else if (mode_ == "model") {
            nmErr_ = 0;   // 非 .nmesh 资产（如 normal 模式 .glb）
        }
        have_ = true;
    }

    // params 统一命名 Map；只发改动过的字段（引擎侧逐字段可选解析）。
    void setMaterial(RpcHost* host, int i, const Row& r) {
        msgpack::Map m;
        m["i"] = (std::int64_t)i;
        nl::editor::msgpack::Array tint;
        tint.push_back((double)r.tint[0]);
        tint.push_back((double)r.tint[1]);
        tint.push_back((double)r.tint[2]);
        m["tint"] = RpcValue(std::move(tint));
        m["rough"] = (double)r.rough;
        m["metal"] = (double)r.metal;
        auto rr = callQuick(host, "scene.setMaterial", RpcValue(std::move(m)));
        logErr(rr, "scene.setMaterial");
    }

    static void logErr(const RpcResult& r, const char* what) {
        if (!std::holds_alternative<RpcError>(r)) return;
        const auto& e = std::get<RpcError>(r);
        ImGui::Text("[%s] error %d: %s", what, e.code, e.message.c_str());
    }

    bool have_ = false;
    std::string mode_ = "?";
    std::vector<Row> rows_;
    int selected_ = -1;
    std::string modelFile_;
    int nmErr_ = -1;                       // -1=无数据 0=非 nmesh 1=读失败
    std::string nmErrText_;
    std::string nmName_;
    long long nmMeshlets_ = 0, nmSubCount_ = 0;
    std::vector<Sub> nmSubs_;
};

void PluginManager::registerPlugin(std::unique_ptr<Plugin> p) {
    plugins_.push_back(std::move(p));
}

void PluginManager::discover() {
    // M-E3/M-E4d/M-E5 内建插件；DLL 动态加载走 discoverDlls()。
    registerPlugin(std::make_unique<CameraPlugin>());
    registerPlugin(std::make_unique<SceneOutlinerPlugin>());
    registerPlugin(std::make_unique<MaterialInspectorPlugin>());
}

// M-E5：plugins/ 目录 DLL 动态扫描。exe 同目录 plugins/*.dll，C ABI 校验
// （abi/create/destroy 三符号 + 版本匹配），任一失败跳过并经 log 上报，
// 绝不让单个坏 DLL 拖死宿主。
void PluginManager::discoverDlls(const std::string& dir,
                                 const std::function<void(const char*)>& log) {
    const std::string pattern = dir + "\\*.dll";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log("plugin dll scan: no *.dll (skipped)");
        return;
    }
    do {
        const std::string name = fd.cFileName;
        const std::string full = dir + "\\" + name;
        HMODULE mod = LoadLibraryA(full.c_str());
        if (!mod) {
            log(("plugin dll " + name + " skipped: LoadLibrary failed").c_str());
            continue;
        }
        auto abi    = (int (*)())GetProcAddress(mod, "nl_plugin_abi");
        auto create = (Plugin* (*)())GetProcAddress(mod, "nl_plugin_create");
        auto destroy = (void (*)(Plugin*))GetProcAddress(mod, "nl_plugin_destroy");
        if (!abi || !create || !destroy) {
            log(("plugin dll " + name + " skipped: missing C ABI exports").c_str());
            FreeLibrary(mod);
            continue;
        }
        if (abi() != kPluginAbiVersion) {
            log(("plugin dll " + name + " skipped: abi version mismatch").c_str());
            FreeLibrary(mod);
            continue;
        }
        Plugin* p = create();
        if (!p) {
            log(("plugin dll " + name + " skipped: create returned null").c_str());
            FreeLibrary(mod);
            continue;
        }
        plugins_.emplace_back(p);
        dlls_.push_back({mod, destroy, plugins_.size() - 1});
        log(("plugin dll loaded: " + name + " (" + p->name() + ")").c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// M-E5：析构对称卸载——DLL 插件必须经 DLL 自己导出的 destroy delete（跨模块
// new/delete 配对），全部摘除后再 FreeLibrary。按 slot 逆序摘，保持下标有效。
PluginManager::~PluginManager() {
    for (auto it = dlls_.rbegin(); it != dlls_.rend(); ++it) {
        if (it->destroy) {
            Plugin* p = plugins_[it->slot].release();
            plugins_.erase(plugins_.begin() + (long long)it->slot);
            it->destroy(p);
        }
        if (it->handle) FreeLibrary(static_cast<HMODULE>(it->handle));
    }
}

void PluginManager::drawAll(RpcHost* host) {
    for (auto& p : plugins_) {
        if (ImGui::Begin(p->name())) p->draw(host);
        ImGui::End();
    }
}

} // namespace nl::editor::ui
