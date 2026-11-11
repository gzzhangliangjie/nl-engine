// vfx-tools —— Effekseer .efkefc → .nfx v1 离线转换器（PT M-PT1，B 类轨道）
//
// 路线 B（用户拍板 2026-11-10）：运行时零 Effekseer 依赖，转换期把参数面折算进
// .nfx 子集；子集外模块 exit 2 显式报错（禁静默丢弃）。
// 解析层 = vendored Effekseer core（MIT，thirdparty/effekseer/VENDOR.md）。
//
// 退出码：0=成功 / 1=断言失败或解析失败 / 2=用法或子集外模块（对齐展示台 expectExit）。
//
// .nfx v1 子集（拍板：基础 sprite 单发射器，反例清单即 exit 2 白名单反面）：
//   - 恰好 1 个 sprite 节点；节点树无 Model/Ribbon/Ring/Track
//   - 生成：Continuous + MaxGeneration==1 + Burst==1 + Point 发射位置（非随机范围）
//   - 平移：Fixed 或 PVA（PVA 加速度须 min==max 折算常加速度；位置须固定点）
//   - 缩放：None/Fixed；颜色：Fixed/Random（RGBA）；billboard；无 UV 动画
//   - 混合：Opacity/Blend/Add；无动态参数引用/力场/碰撞/音效/GPU 粒子/KillRules
//   - 贴图只记路径（runtime stb 加载，缺文件回退程序化软圆盘）
//
// 坐标口径：Effekseer 默认 Y-up 与本引擎 sim 同轴（重力 -Y）；若样例实测 Z-up
// 偏差，转换期加轴映射并在 VFX-TOOLS-GUIDE 落档（不进运行时）。

#include <Effekseer.h>
#include <Effekseer/Effekseer.EffectNode.h>
#include <Effekseer/Effekseer.EffectNodeSprite.h>
#include <Effekseer/IO/Effekseer.EfkEfcFactory.h>

#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <array>

using json = nlohmann::json;

namespace fs = std::filesystem;

namespace
{

// 子集外失败通道：收集原因清单统一 exit 2（禁静默丢弃）
struct Unsupported
{
    std::vector<std::string> reasons;
    void add(const std::string& r) { reasons.push_back(r); }
    bool empty() const { return reasons.empty(); }
};

[[noreturn]] void usageFail(const char* msg)
{
    fprintf(stderr, "usage error: %s\n", msg);
    fprintf(stderr, "vfx-tools convert <in.efkefc> <out.nfx>\n");
    fprintf(stderr, "vfx-tools selftest\n");
    exit(2);
}

std::vector<uint8_t> readFileOrDie(const fs::path& p)
{
    FILE* f = nullptr;
#ifdef _WIN32
    _wfopen_s(&f, p.c_str(), L"rb");
#else
    f = fopen(p.string().c_str(), "rb");
#endif
    if (!f)
    {
        fprintf(stderr, "error: cannot open '%s'\n", p.string().c_str());
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0 && fread(buf.data(), 1, buf.size(), f) != buf.size())
    {
        fclose(f);
        fprintf(stderr, "error: short read '%s'\n", p.string().c_str());
        exit(2);
    }
    fclose(f);
    return buf;
}

// 桩 loader：转换器只要路径（Effect::Get*ImagePath 公开 API），不碰磁盘资源——
// 全部返回 nullptr（等同"文件缺失"，Effect 解析不受影响）。
class StubTextureLoader : public Effekseer::TextureLoader
{
    Effekseer::TextureRef Load(const char16_t*, Effekseer::TextureType) override { return nullptr; }
    Effekseer::TextureRef Load(const void*, int32_t, Effekseer::TextureType, bool) override { return nullptr; }
};
class StubModelLoader : public Effekseer::ModelLoader
{
    Effekseer::ModelRef Load(const char16_t*) override { return nullptr; }
    Effekseer::ModelRef Load(const void*, int32_t) override { return nullptr; }
};
class StubSoundLoader : public Effekseer::SoundLoader
{
    Effekseer::SoundDataRef Load(const char16_t*) override { return nullptr; }
    Effekseer::SoundDataRef Load(const void*, int32_t) override { return nullptr; }
};
class StubMaterialLoader : public Effekseer::MaterialLoader
{
    Effekseer::MaterialRef Load(const char16_t*) override { return nullptr; }
    Effekseer::MaterialRef Load(const void*, int32_t, Effekseer::MaterialFileType) override { return nullptr; }
};
class StubCurveLoader : public Effekseer::CurveLoader
{
    Effekseer::CurveRef Load(const char16_t*) override { return nullptr; }
    Effekseer::CurveRef Load(const void*, int32_t) override { return nullptr; }
};

void u16ToUtf8(const char16_t* src, std::string& out)
{
    out.clear();
    if (!src)
        return;
    while (*src)
    {
        uint32_t cp = *src++;
        if (cp >= 0xD800 && cp < 0xDC00 && *src)
        { // surrogate pair
            cp = 0x10000 + ((cp - 0xD800) << 10) + (*src++ - 0xDC00);
        }
        if (cp < 0x80)
            out.push_back(char(cp));
        else if (cp < 0x800)
        {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }
}

struct NfxDoc
{
    int efkVersion = 0;
    // emitter
    float rateMin = 0, rateMax = 0; // particles/s
    float lifeMin = 0, lifeMax = 0; // seconds
    float spawnPoint[3] = {0, 0, 0};
    float velMin[3] = {0, 0, 0};
    float velMax[3] = {0, 0, 0};
    float accel[3] = {0, 0, 0}; // 常加速度（PVA accel min==max 折算）
    // render
    std::string blend; // "alpha" | "additive"
    int colorStart[4] = {255, 255, 255, 255};
    int colorEnd[4] = {255, 255, 255, 255};
    float sizeX = 1.0f, sizeY = 1.0f;
    float fadeIn = 0.0f, fadeOut = 0.0f; // seconds
    std::string texture;                 // 相对路径，"" = 无

    // M-PT3：flipbook 序列帧（.nfx v1 可选字段；缺省 = 无）。UV 口径对齐
    // Effekseer UVAnimationType::Animation：frameLength 单位 = Efke 帧（1/60s），
    // offset/cell 是贴图 UV（V0 = 贴图顶部），帧序 = 行主序（frameX 先走）。
    struct Flipbook
    {
        bool  enabled = false;
        float frameLength = 1.0f; // 帧数（Effekseer 帧），>0 且非极大值
        int   countX = 1, countY = 1;
        int   loopType = 0; // 0=once 1=loop 2=reverse（Effekseer 原值）
        float offsetU = 0.0f, offsetV = 0.0f; // Position.x/.y
        float cellW = 1.0f, cellH = 1.0f;     // Position.w/.h
    } flipbook;

    // M-PT4 E2：渐变颜色（ColorAll Easing 烘焙 / Gradient 直映射；缺省 = 无）。
    // 关键帧 {t, v...}，t ∈ [0,1] 非降序；v 单位 0..255（与 colorStart/colorEnd
    // 同单位，运行时 /255 归一化）。
    struct ColorCurve
    {
        bool enabled = false;
        std::vector<std::array<float, 4>> colorKeys; // {t, r, g, b}
        std::vector<std::array<float, 2>> alphaKeys; // {t, a}
    } colorCurve;

    // M-PT5 E5：旋转（Rotation None/Fixed/PVA 的 Z 分量；缺省 rotEnabled=false）。
    // 口径对齐 vendored Effekseer runtime：加载链无度→弧度转换，
    // CalculateRotation 直接吃弧度（Mat43f::RotationZXY）→ .nfx 存弧度。
    // billboard 子集只认屏幕平面 Z：PVA 的 rotation/velocity x/y 分量与
    // acceleration 非零、Easing/AxisPVA/AxisEasing/FCurve/RotateToViewpoint/
    // Velocity 全部 exit 2。
    bool  rotEnabled = false;
    float angMin = 0.0f, angMax = 0.0f;         // 初始角（弧度，PVA rotation.z）
    float angVelMin = 0.0f, angVelMax = 0.0f;   // 角速度（弧度/秒，PVA velocity.z）

    // M-PT5 R1：per-particle 随机 sizeY（Scaling PVA 的 y 区间；缺省 = 无）。
    // 绝对米口径（与 size 同单位）；X 必须定值（position.x min==max），
    // scaling 的 velocity/acceleration 非零 exit 2。运行时折算 per-particle
    // Y/X 比进实例流（VS 弃 CB 标量 g_params.z）。
    bool  sizeYRangeEnabled = false;
    float sizeYRangeMin = 0.0f, sizeYRangeMax = 0.0f;
};

json nfxToJson(const NfxDoc& d, const std::string& sourceName)
{
    json j;
    j["magic"] = "NFX1";
    j["source"] = sourceName;
    j["efkVersion"] = d.efkVersion;
    j["emitter"] = {
        {"rate", {d.rateMin, d.rateMax}},
        {"life", {d.lifeMin, d.lifeMax}},
        {"spawnPoint", {d.spawnPoint[0], d.spawnPoint[1], d.spawnPoint[2]}},
        {"velocityMin", {d.velMin[0], d.velMin[1], d.velMin[2]}},
        {"velocityMax", {d.velMax[0], d.velMax[1], d.velMax[2]}},
        {"accel", {d.accel[0], d.accel[1], d.accel[2]}},
    };
    // M-PT5 E5：旋转（缺省/None 不写字段 = 老格式零改动兼容）
    if (d.rotEnabled)
    {
        j["emitter"]["ang"]    = {d.angMin, d.angMax};
        j["emitter"]["angVel"] = {d.angVelMin, d.angVelMax};
    }
    j["render"] = {
        {"blend", d.blend},
        {"colorStart", {d.colorStart[0], d.colorStart[1], d.colorStart[2], d.colorStart[3]}},
        {"colorEnd", {d.colorEnd[0], d.colorEnd[1], d.colorEnd[2], d.colorEnd[3]}},
        // M-PT5 R1：sizeYRange 启用时 Y 语义由 sizeYRange 承担，size[1] 写
        // sizeX 占位（方形折算 ratioY=1.0 被运行时 sizeYRange 覆盖）
        {"size", {d.sizeX, d.sizeYRangeEnabled ? d.sizeX : d.sizeY}},
        {"fadeIn", d.fadeIn},
        {"fadeOut", d.fadeOut},
        {"texture", d.texture.empty() ? json(nullptr) : json(d.texture)},
    };
    if (d.flipbook.enabled)
    {
        j["render"]["flipbook"] = {
            {"frameLength", d.flipbook.frameLength},
            {"countX", d.flipbook.countX},
            {"countY", d.flipbook.countY},
            {"loop", d.flipbook.loopType}, // 0=once 1=loop 2=reverse
            {"offset", {d.flipbook.offsetU, d.flipbook.offsetV}},
            {"cell", {d.flipbook.cellW, d.flipbook.cellH}},
        };
    }
    // M-PT4 E2：可选 colorCurve（缺省 = 无，v1 老文件零改动兼容）
    if (d.colorCurve.enabled)
    {
        json ck = json::array();
        for (const auto& k : d.colorCurve.colorKeys)
            ck.push_back({k[0], k[1], k[2], k[3]});
        json ak = json::array();
        for (const auto& k : d.colorCurve.alphaKeys)
            ak.push_back({k[0], k[1]});
        j["render"]["colorCurve"] = {{"colorKeys", ck}, {"alphaKeys", ak}};
    }
    // M-PT5 R1：可选 sizeYRange（缺省 = 无，per-emitter size[1] 口径不变）
    if (d.sizeYRangeEnabled)
    {
        j["render"]["sizeYRange"] = {d.sizeYRangeMin, d.sizeYRangeMax};
    }
    return j;
}

// nfxToJson 的镜像读回（selftest 往返用）
bool nfxFromJson(const json& j, NfxDoc& d, std::string& err)
{
    auto get3 = [&](const json& o, const char* key, float* v) -> bool {
        if (!o.contains(key) || !o[key].is_array() || o[key].size() != 3)
            return false;
        for (int a = 0; a < 3; a++)
            v[a] = o[key][a].get<float>();
        return true;
    };
    auto get4i = [&](const json& o, const char* key, int* v) -> bool {
        if (!o.contains(key) || !o[key].is_array() || o[key].size() != 4)
            return false;
        for (int c = 0; c < 4; c++)
            v[c] = o[key][c].get<int>();
        return true;
    };
    if (j.value("magic", "") != "NFX1")
    {
        err = "bad magic";
        return false;
    }
    d.efkVersion = j.value("efkVersion", 0);
    const json& e = j.at("emitter");
    if (!e.contains("rate") || e["rate"].size() != 2 || !e.contains("life") || e["life"].size() != 2)
    {
        err = "bad emitter arrays";
        return false;
    }
    d.rateMin = e["rate"][0].get<float>();
    d.rateMax = e["rate"][1].get<float>();
    d.lifeMin = e["life"][0].get<float>();
    d.lifeMax = e["life"][1].get<float>();
    if (!get3(e, "spawnPoint", d.spawnPoint) || !get3(e, "velocityMin", d.velMin) ||
        !get3(e, "velocityMax", d.velMax) || !get3(e, "accel", d.accel))
    {
        err = "bad emitter vec3";
        return false;
    }
    const json& r = j.at("render");
    d.blend = r.value("blend", "alpha");
    if (!get4i(r, "colorStart", d.colorStart) || !get4i(r, "colorEnd", d.colorEnd))
    {
        err = "bad color arrays";
        return false;
    }
    if (!r.contains("size") || r["size"].size() != 2)
    {
        err = "bad size";
        return false;
    }
    d.sizeX = r["size"][0].get<float>();
    d.sizeY = r["size"][1].get<float>();
    d.fadeIn = r.value("fadeIn", 0.0f);
    d.fadeOut = r.value("fadeOut", 0.0f);
    d.texture = r.value("texture", json(nullptr)) == json(nullptr) ? "" : r["texture"].get<std::string>();
    // M-PT3：flipbook 可选字段（缺省 = 无，v1 老文件零改动兼容）
    if (r.contains("flipbook"))
    {
        const json& fb = r["flipbook"];
        if (!fb.is_object() || !fb.contains("frameLength") || !fb.contains("countX") ||
            !fb.contains("countY") || !fb.contains("loop") || !fb.contains("offset") ||
            !fb.contains("cell"))
        {
            err = "bad flipbook object";
            return false;
        }
        d.flipbook.enabled = true;
        d.flipbook.frameLength = fb["frameLength"].get<float>();
        d.flipbook.countX = fb["countX"].get<int>();
        d.flipbook.countY = fb["countY"].get<int>();
        d.flipbook.loopType = fb["loop"].get<int>();
        d.flipbook.offsetU = fb["offset"][0].get<float>();
        d.flipbook.offsetV = fb["offset"][1].get<float>();
        d.flipbook.cellW = fb["cell"][0].get<float>();
        d.flipbook.cellH = fb["cell"][1].get<float>();
    }
    // M-PT4 E2：可选 colorCurve 读回（校验与运行时 loadNfx 同口径：t ∈ [0,1]
    // 非降序、键值 0..255、数组长度恰为 4/2）
    if (r.contains("colorCurve"))
    {
        const json& cc = r["colorCurve"];
        if (!cc.is_object() || !cc.contains("colorKeys") || !cc.contains("alphaKeys") ||
            !cc["colorKeys"].is_array() || !cc["alphaKeys"].is_array() ||
            cc["colorKeys"].empty() || cc["alphaKeys"].empty())
        {
            err = "bad colorCurve object";
            return false;
        }
        auto parseKeys = [](const json& arr, int nch, std::vector<std::array<float, 4>>& out4,
                            std::vector<std::array<float, 2>>& out2, bool isColor) -> bool {
            float prevT = -1.0f;
            for (const json& k : arr)
            {
                if (!k.is_array() || static_cast<int>(k.size()) != nch + 1)
                    return false;
                const float t = k[0].get<float>();
                if (!(t >= 0.0f && t <= 1.0f) || t < prevT)
                    return false;
                if (isColor)
                {
                    std::array<float, 4> v = {t, 0.0f, 0.0f, 0.0f};
                    for (int c = 0; c < 3; c++)
                    {
                        const float x = k[c + 1].get<float>();
                        if (!(x >= 0.0f && x <= 255.0f))
                            return false;
                        v[c + 1] = x;
                    }
                    out4.push_back(v);
                }
                else
                {
                    const float a = k[1].get<float>();
                    if (!(a >= 0.0f && a <= 255.0f))
                        return false;
                    out2.push_back({t, a});
                }
                prevT = t;
            }
            return true;
        };
        if (!parseKeys(cc["colorKeys"], 3, d.colorCurve.colorKeys, d.colorCurve.alphaKeys, true) ||
            !parseKeys(cc["alphaKeys"], 1, d.colorCurve.colorKeys, d.colorCurve.alphaKeys, false))
        {
            err = "colorCurve keys out of range";
            return false;
        }
        d.colorCurve.enabled = true;
    }
    // M-PT5 E5：可选旋转读回（emitter.ang / emitter.angVel，长度 2 弧度区间）
    if (e.contains("ang") || e.contains("angVel"))
    {
        if (!e.contains("ang") || !e.contains("angVel") || !e["ang"].is_array() ||
            e["ang"].size() != 2 || !e["angVel"].is_array() || e["angVel"].size() != 2)
        {
            err = "bad ang/angVel arrays";
            return false;
        }
        d.rotEnabled = true;
        d.angMin = e["ang"][0].get<float>();
        d.angMax = e["ang"][1].get<float>();
        d.angVelMin = e["angVel"][0].get<float>();
        d.angVelMax = e["angVel"][1].get<float>();
    }
    // M-PT5 R1：可选 sizeYRange 读回（绝对米区间，两元素）
    if (r.contains("sizeYRange"))
    {
        if (!r["sizeYRange"].is_array() || r["sizeYRange"].size() != 2)
        {
            err = "bad sizeYRange array";
            return false;
        }
        d.sizeYRangeEnabled = true;
        d.sizeYRangeMin = r["sizeYRange"][0].get<float>();
        d.sizeYRangeMax = r["sizeYRange"][1].get<float>();
    }
    return true;
}

// 主提取：拍板子集的反例全部收集进 unsup；返回值仅在 unsup 空时有效
NfxDoc extract(const Effekseer::Effect* effect, Unsupported& unsup)
{
    using namespace Effekseer;
    NfxDoc d;
    d.efkVersion = effect->GetVersion();

    if (effect->GetMaginification() != 1.0f)
        unsup.add("magnification != 1 (v1 does not rescale)");

    auto rootImpl = static_cast<EffectNodeImplemented*>(effect->GetRoot());
    int spriteCount = 0;
    EffectNodeImplemented* sprite = nullptr;
    rootImpl->Traverse([&](EffectNodeImplemented* n) {
        auto t = n->GetType();
        if (t == EffectNodeType::Root)
            return true;
        if (t == EffectNodeType::Sprite)
        {
            spriteCount++;
            if (!sprite)
                sprite = n;
            return true;
        }
        const char* names[] = {"none", "?", "sprite", "ribbon", "ring", "model", "track"};
        unsup.add(std::string("node type '") + (t >= EffectNodeType::NoneType && t <= EffectNodeType::Track
                                                   ? names[int(t)]
                                                   : "?") +
                  "' not supported (v1 = single sprite emitter)");
        return true;
    });
    if (!unsup.empty())
        return d;
    if (spriteCount != 1 || !sprite)
    {
        unsup.add("expected exactly 1 sprite node, found " + std::to_string(spriteCount));
        return d;
    }

    const EffectNodeImplemented& n = *sprite;
    const auto& cv = n.CommonValues;
    if (cv.Generation.Type != GenerationTiming::Continuous)
        unsup.add("generation timing != Continuous");
    if (cv.MaxGeneration != 1)
        unsup.add("MaxGeneration != 1 (looped emitter)");
    if (cv.Generation.Burst.min != 1 || cv.Generation.Burst.max != 1)
        unsup.add("Burst != 1");
    if (cv.RefEqLife.Max >= 0 || cv.Generation.RefEqInterval.Max >= 0 ||
        cv.Generation.RefEqOffset.Max >= 0 || cv.Generation.RefEqBurst.Max >= 0 ||
        cv.RefEqMaxGeneration >= 0)
        unsup.add("dynamic parameter reference (RefEq) on common values");
    if (cv.Removal.Flags != RemovalTiming::WhenLifeIsExtinct)
        unsup.add("removal timing != WhenLifeIsExtinct");

    // 发射位置：Point only（sphere/circle/line/model → 子集外）
    if (n.GenerationLocation.type != ParameterGenerationLocation::TYPE_POINT)
    {
        unsup.add("generation location != Point (sphere/circle/line/model)");
    }
    else
    {
        const auto& loc = n.GenerationLocation.point.location;
        for (int a = 0; a < 3; a++)
        {
            float mn = (&loc.min.x)[a], mx = (&loc.max.x)[a];
            d.spawnPoint[a] = 0.5f * (mn + mx);
            if (mn != mx)
                unsup.add("spawn point is a random range (v1 = fixed point)");
        }
    }

    // 平移：Fixed 或 PVA
    const auto& tp = n.TranslationParam;
    if (tp.TranslationType == ParameterTranslationType_Fixed)
    {
        for (int a = 0; a < 3; a++)
            d.spawnPoint[a] += (&tp.TranslationFixed.Position.X)[a];
    }
    else if (tp.TranslationType == ParameterTranslationType_PVA)
    {
        const auto& pva = tp.TranslationPVA;
        if (pva.RefEqV.Max >= 0 || pva.RefEqA.Max >= 0 || pva.RefEqP.Max >= 0)
            unsup.add("dynamic parameter reference (RefEq) on translation PVA");
        for (int a = 0; a < 3; a++)
        {
            d.velMin[a] = (&pva.velocity.min.x)[a];
            d.velMax[a] = (&pva.velocity.max.x)[a];
            float aMin = (&pva.acceleration.min.x)[a], aMax = (&pva.acceleration.max.x)[a];
            if (aMin != aMax)
                unsup.add("PVA acceleration is a random range (v1 = constant acceleration)");
            d.accel[a] = aMin;
            float mn = (&pva.location.min.x)[a], mx = (&pva.location.max.x)[a];
            if (mn != mx)
                unsup.add("PVA initial location is a random range (v1 = fixed spawn point)");
        }
    }
    else
    {
        unsup.add("translation type != Fixed/PVA (Easing/FCurve/Nurbs/ViewOffset)");
    }

    // 生成率与寿命（Effekseer 时间单位 = 帧，1/60s）
    constexpr float kFrame = 1.0f / 60.0f;
    if (cv.Generation.Interval.min <= 0 || cv.Generation.Interval.max <= 0)
        unsup.add("generation interval <= 0");
    else
    {
        d.rateMin = 60.0f / cv.Generation.Interval.max;
        d.rateMax = 60.0f / cv.Generation.Interval.min;
    }
    d.lifeMin = float(cv.life.min) * kFrame;
    d.lifeMax = float(cv.life.max) * kFrame;

    // 缩放：None/Fixed/PVA（M-PT5 R1：PVA 的 y 区间收进 per-particle sizeYRange；
    // X 必须定值 position.x min==max；scaling velocity/acceleration 非零 exit 2）
    const auto& sp = n.ScalingParam;
    if (sp.ScalingType == ParameterScalingType::ParameterScalingType_None)
    {
        d.sizeX = d.sizeY = 1.0f;
    }
    else if (sp.ScalingType == ParameterScalingType::ParameterScalingType_Fixed)
    {
        d.sizeX = sp.ScalingFixed.Position.X;
        d.sizeY = sp.ScalingFixed.Position.Y;
        if (sp.ScalingFixed.RefEq >= 0)
            unsup.add("dynamic parameter reference (RefEq) on scaling");
    }
    else if (sp.ScalingType == ParameterScalingType::ParameterScalingType_PVA)
    {
        const auto& pos = sp.ScalingPVA.Position;
        const auto& sv = sp.ScalingPVA.Velocity;
        const auto& sa = sp.ScalingPVA.Acceleration;
        if (pos.min.x != pos.max.x)
            unsup.add("scaling PVA X range != 0 (R1 subset is Y-only random)");
        if (sv.min.x != 0.0f || sv.max.x != 0.0f || sv.min.y != 0.0f || sv.max.y != 0.0f ||
            sv.min.z != 0.0f || sv.max.z != 0.0f ||
            sa.min.x != 0.0f || sa.max.x != 0.0f || sa.min.y != 0.0f || sa.max.y != 0.0f ||
            sa.min.z != 0.0f || sa.max.z != 0.0f)
            unsup.add("scaling PVA velocity/acceleration != 0");
        if (sp.ScalingPVA.RefEqP.Min >= 0 || sp.ScalingPVA.RefEqV.Min >= 0 ||
            sp.ScalingPVA.RefEqA.Min >= 0)
            unsup.add("dynamic parameter reference (RefEq) on scaling PVA");
        d.sizeX = pos.min.x;              // X 定值（min==max 已校验）
        d.sizeYRangeEnabled = true;
        d.sizeYRangeMin = pos.min.y;
        d.sizeYRangeMax = pos.max.y;
    }
    else
    {
        unsup.add("scaling type != None/Fixed/PVA (Easing/FCurve)");
    }

    // 渲染公共段
    const auto& rc = n.RendererCommon;
    if (rc.MaterialType != RendererMaterialType::Default)
        unsup.add("material type != Default (File/BackDistortion/Lighting material)");
    if (rc.AlphaBlend == AlphaBlendType::Opacity || rc.AlphaBlend == AlphaBlendType::Blend)
        d.blend = "alpha";
    else if (rc.AlphaBlend == AlphaBlendType::Add)
        d.blend = "additive";
    else
        unsup.add("alpha blend != Opacity/Blend/Add (Sub/Mul)");
    if (rc.ColorBindType == BindType::Always || rc.ColorBindType == BindType::WhenCreating)
        unsup.add("color inheritance (ColorBind) not supported");
    if (rc.FadeInType == ParameterRendererCommon::FADEIN_ON)
        d.fadeIn = rc.FadeIn.Frame * kFrame;
    if (rc.FadeOutType == ParameterRendererCommon::FADEOUT_WITHIN_LIFETIME)
        d.fadeOut = rc.FadeOut.Frame * kFrame;
    for (size_t uvi = 0; uvi < rc.UVs.size() && uvi < 4; uvi++)
    {
        const auto& uv = rc.UVs[uvi];
        if (uv.Type == UVAnimationType::Default)
            continue;
        // M-PT3：sprite 主 UV（[0]）支持 Animation 子集（flipbook 序列帧）；
        // 此处只有 sprite 节点能到达，其余 UV 通道与其它动画类型仍不支持。
        if (uvi == 0 && uv.Type == UVAnimationType::Animation)
        {
            const auto& anim = uv.Animation;
            // 子集边界：StartFrame 必须恒 0（random {max,min} 序）、无帧间插值、
            // FrameLength 正数且非极大值（极大 = 固定帧，等价 Fixed UV，拒绝）。
            if (anim.StartFrame.max != 0 || anim.StartFrame.min != 0)
                unsup.add("flipbook StartFrame != 0 (random start)");
            if (anim.InterpolationType != anim.NONE)
                unsup.add("flipbook frame interpolation (LERP) not supported");
            if (anim.FrameLength <= 0 ||
                UVFunctions::IsInfiniteValue(anim.FrameLength))
                unsup.add("flipbook FrameLength <= 0 or infinite");
            if (anim.FrameCountX <= 0 || anim.FrameCountY <= 0)
                unsup.add("flipbook FrameCount <= 0");
            if (!unsup.empty())
                continue;
            d.flipbook.enabled = true;
            d.flipbook.frameLength = float(anim.FrameLength);
            d.flipbook.countX = anim.FrameCountX;
            d.flipbook.countY = anim.FrameCountY;
            d.flipbook.loopType = anim.LoopType;
            d.flipbook.offsetU = anim.Position.x;
            d.flipbook.offsetV = anim.Position.y;
            d.flipbook.cellW = anim.Position.w;
            d.flipbook.cellH = anim.Position.h;
            continue;
        }
        const char* typeName = uv.Type == UVAnimationType::Fixed   ? "Fixed"
                               : uv.Type == UVAnimationType::Scroll ? "Scroll"
                               : uv.Type == UVAnimationType::FCurve ? "FCurve"
                                                                    : "Animation";
        unsup.add((std::string("UV animation != Default on channel ") +
                   std::to_string(uvi) + " (" + typeName +
                   (uvi == 0 ? ", subset violated)" : ")"))
                      .c_str());
    }

    // sprite 专属段
    const auto& sn = *static_cast<const EffectNodeSprite*>(sprite);
    if (sn.Billboard != BillboardType::Billboard)
        unsup.add("billboard type != Billboard (Y-axis/fixed/rotated/directional)");

    const auto& ac = sn.SpriteAllColor;
    if (ac.type == AllTypeColorParameter::Fixed)
    {
        for (int c = 0; c < 4; c++)
            d.colorStart[c] = d.colorEnd[c] = (&ac.fixed.all.R)[c];
    }
    else if (ac.type == AllTypeColorParameter::Random)
    {
        if (ac.random.all.mode == COLOR_MODE_HSVA)
            unsup.add("random color mode HSVA (v1 = RGBA)");
        else
        {
            for (int c = 0; c < 4; c++)
            {
                d.colorStart[c] = (&ac.random.all.min.R)[c];
                d.colorEnd[c] = (&ac.random.all.max.R)[c];
            }
        }
    }
    else if (ac.type == AllTypeColorParameter::Easing)
    {
        // M-PT4 E2：Easing 颜色子集——start/end 定值 + RGBA 模式，d = A·t³+B·t²+C·t
        // （easing_color::setValueToArg 同式）烘焙成 17 个关键帧（对 8bit 输出量化
        // 误差可忽略）。HSVA 模式与 per-particle 随机 start/end（单发射器关键帧
        // 无法表达逐粒子曲线）仍子集外 exit 2。
        const auto& ec = ac.easing.all;
        if (ec.start.mode != COLOR_MODE_RGBA || ec.end.mode != COLOR_MODE_RGBA)
            unsup.add("easing color mode != RGBA (HSVA)");
        else if (!(ec.start.max == ec.start.min) || !(ec.end.max == ec.end.min))
            unsup.add("easing color start/end is random per particle (baked curve = fixed only)");
        else
        {
            constexpr int kBake = 17;
            const Color s = ec.start.min, e = ec.end.min;
            auto lerpCh = [s, e](int ch, float dd) {
                const float s0 = (&s.R)[ch], e0 = (&e.R)[ch];
                const float v = s0 + (e0 - s0) * dd;
                return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
            };
            for (int i = 0; i < kBake; ++i)
            {
                const float t = float(i) / float(kBake - 1);
                const float dd = ec.easingA * t * t * t + ec.easingB * t * t + ec.easingC * t;
                d.colorCurve.colorKeys.push_back(
                    {t, lerpCh(0, dd), lerpCh(1, dd), lerpCh(2, dd)});
                d.colorCurve.alphaKeys.push_back({t, lerpCh(3, dd)});
            }
            d.colorCurve.enabled = true;
            const auto& f = d.colorCurve.colorKeys.front();
            const auto& l = d.colorCurve.colorKeys.back();
            const auto& fa = d.colorCurve.alphaKeys.front();
            const auto& la = d.colorCurve.alphaKeys.back();
            for (int c = 0; c < 3; c++)
            {
                d.colorStart[c] = int(f[c + 1] + 0.5f);
                d.colorEnd[c] = int(l[c + 1] + 0.5f);
            }
            d.colorStart[3] = int(fa[1] + 0.5f);
            d.colorEnd[3] = int(la[1] + 0.5f);
        }
    }
    else if (ac.type == AllTypeColorParameter::Gradient_)
    {
        // M-PT4 E2：Gradient 颜色子集——直映射。GetColor = lerp(color)×lerp(intensity)
        // 分段线性（端点 clamp，0 color 键 = 白、0 alpha 键 = 1）：intensity 全等时
        // 折进色值逐键精确；非均匀 intensity 是二次曲线，线性关键帧无法精确表达。
        const auto& g = *ac.gradient;
        bool uniform = true;
        for (int i = 1; i < g.ColorCount; i++)
            if (g.Colors[i].Intensity != g.Colors[0].Intensity)
                uniform = false;
        auto sorted = [](const auto* keys, int n) {
            for (int i = 1; i < n; i++)
                if (keys[i].Position < keys[i - 1].Position)
                    return false;
            return true;
        };
        if (!uniform)
            unsup.add("gradient color intensity non-uniform (linear keyframes cannot express lerped-color x lerped-intensity)");
        if (!sorted(g.Colors.data(), g.ColorCount) || !sorted(g.Alphas.data(), g.AlphaCount))
            unsup.add("gradient keys unsorted (GetColor lower_bound assumes order)");
        if (unsup.empty())
        {
            const float k = g.Colors[0].Intensity;
            if (g.ColorCount == 0)
            {
                d.colorCurve.colorKeys.push_back({0.0f, 255.0f, 255.0f, 255.0f});
            }
            else
            {
                for (int i = 0; i < g.ColorCount; i++)
                    d.colorCurve.colorKeys.push_back(
                        {g.Colors[i].Position, g.Colors[i].Color[0] * k * 255.0f,
                         g.Colors[i].Color[1] * k * 255.0f, g.Colors[i].Color[2] * k * 255.0f});
            }
            if (g.AlphaCount == 0)
                d.colorCurve.alphaKeys.push_back({0.0f, 255.0f});
            else
                for (int i = 0; i < g.AlphaCount; i++)
                    d.colorCurve.alphaKeys.push_back(
                        {g.Alphas[i].Position, g.Alphas[i].Alpha * 255.0f});
            d.colorCurve.enabled = true;
            const auto& f = d.colorCurve.colorKeys.front();
            const auto& l = d.colorCurve.colorKeys.back();
            const auto& fa = d.colorCurve.alphaKeys.front();
            const auto& la = d.colorCurve.alphaKeys.back();
            for (int c = 0; c < 3; c++)
            {
                d.colorStart[c] = int(std::min(255.0f, std::max(0.0f, f[c + 1])) + 0.5f);
                d.colorEnd[c] = int(std::min(255.0f, std::max(0.0f, l[c + 1])) + 0.5f);
            }
            d.colorStart[3] = int(std::min(255.0f, std::max(0.0f, fa[1])) + 0.5f);
            d.colorEnd[3] = int(std::min(255.0f, std::max(0.0f, la[1])) + 0.5f);
        }
    }
    else
    {
        unsup.add("color type != Fixed/Random/Easing/Gradient (FCurve)");
    }

    // 尺寸四角：Default 加载期已归一化为 ±0.5；Fixed 取轴对齐宽高
    const auto& pos = sn.SpritePosition;
    if (pos.type == SpritePositionParameter::Fixed)
    {
        float llX = pos.fixed.ll.GetX(), llY = pos.fixed.ll.GetY();
        float lrX = pos.fixed.lr.GetX(), lrY = pos.fixed.lr.GetY();
        float ulX = pos.fixed.ul.GetX(), ulY = pos.fixed.ul.GetY();
        float urX = pos.fixed.ur.GetX(), urY = pos.fixed.ur.GetY();
        float w = lrX - llX;
        float h = ulY - llY;
        if (llY != lrY || ulY != urY || llX != ulX || lrX != urX)
        {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "sprite quad is not axis-aligned (ll=%.2f,%.2f lr=%.2f,%.2f ul=%.2f,%.2f ur=%.2f,%.2f)",
                     llX, llY, lrX, lrY, ulX, ulY, urX, urY);
            unsup.add(buf);
        }
        d.sizeX *= (w > 0 ? w : 1.0f);
        d.sizeY *= (h > 0 ? h : 1.0f);
    }

    // 反例清单：力场/碰撞/音效/GPU 粒子/KillRules/LOD/Falloff/AlphaCutoff
    // M-PT5 E5：旋转放宽为 None/Fixed/PVA 的屏幕平面 Z 分量（billboard 子集）。
    // 口径 = vendored runtime：加载链无度→弧度，.nfx 直存弧度。
    const auto& rp = n.RotationParam;
    if (rp.RotationType == ParameterRotationType::ParameterRotationType_None)
    {
        // 无旋转（缺省）
    }
    else if (rp.RotationType == ParameterRotationType::ParameterRotationType_Fixed)
    {
        if (rp.RotationFixed.RefEq >= 0)
            unsup.add("dynamic parameter reference (RefEq) on rotation");
        d.rotEnabled = true;
        d.angMin = d.angMax = rp.RotationFixed.Position.Z;
        if (rp.RotationFixed.Position.X != 0.0f || rp.RotationFixed.Position.Y != 0.0f)
            unsup.add("rotation Fixed X/Y != 0 (billboard subset is Z-only)");
    }
    else if (rp.RotationType == ParameterRotationType::ParameterRotationType_PVA)
    {
        const auto& rot = rp.RotationPVA.rotation;
        const auto& vel = rp.RotationPVA.velocity;
        const auto& acc = rp.RotationPVA.acceleration;
        if (rot.min.x != 0.0f || rot.max.x != 0.0f || rot.min.y != 0.0f || rot.max.y != 0.0f)
            unsup.add("rotation PVA X/Y range != 0 (billboard subset is Z-only)");
        if (vel.min.x != 0.0f || vel.max.x != 0.0f || vel.min.y != 0.0f || vel.max.y != 0.0f)
            unsup.add("rotation velocity X/Y range != 0 (billboard subset is Z-only)");
        if (acc.min.z != 0.0f || acc.max.z != 0.0f || acc.min.x != 0.0f || acc.max.x != 0.0f ||
            acc.min.y != 0.0f || acc.max.y != 0.0f)
            unsup.add("rotation PVA acceleration != 0");
        if (rp.RotationPVA.RefEqP.Min >= 0 || rp.RotationPVA.RefEqV.Min >= 0 ||
            rp.RotationPVA.RefEqA.Min >= 0)
            unsup.add("dynamic parameter reference (RefEq) on rotation PVA");
        d.rotEnabled = true;
        d.angMin = rot.min.z;
        d.angMax = rot.max.z;
        d.angVelMin = vel.min.z;
        d.angVelMax = vel.max.z;
    }
    else
    {
        unsup.add("rotation type != None/Fixed/PVA (Easing/AxisPVA/AxisEasing/FCurve/RotateToViewpoint/Velocity)");
    }
    if (n.LocalForceField.HasValue || n.LocalForceField.IsGlobalEnabled)
        unsup.add("force field enabled");
    if (n.Collisions.IsGroundCollisionEnabled)
        unsup.add("collision enabled");
    if (n.Sound.SoundType != ParameterSoundType_None)
        unsup.add("sound enabled");
    if (n.GpuParticlesResource != nullptr)
        unsup.add("GPU particles module");
    if (n.KillParam.Type != KillType::None)
        unsup.add("kill rules enabled");
    if (n.EnableFalloff)
        unsup.add("falloff enabled");
    if (n.AlphaCutoff.Type != ParameterAlphaCutoff::EType::FIXED || n.AlphaCutoff.Fixed.RefEq >= 0 ||
        n.AlphaCutoff.Fixed.Threshold != 0.0f)
        unsup.add("alpha cutoff != off");

    // 贴图路径（只记名字，runtime stb 加载；缺文件回退程序化软圆盘）
    int texIdx = n.RendererCommon.TextureIndexes[0];
    if (texIdx >= 0 && texIdx < effect->GetColorImageCount())
    {
        std::string p;
        u16ToUtf8(effect->GetColorImagePath(texIdx), p);
        d.texture = p;
    }
    return d;
}

int runConvert(const fs::path& inPath, const fs::path& outPath)
{
    std::vector<uint8_t> data = readFileOrDie(inPath);

    auto setting = Effekseer::Setting::Create(); // Setting.cpp 默认注册 EfkEfcFactory（SKFE 格式路由）
    setting->SetTextureLoader(Effekseer::TextureLoaderRef(new StubTextureLoader()));
    setting->SetModelLoader(Effekseer::ModelLoaderRef(new StubModelLoader()));
    setting->SetSoundLoader(Effekseer::SoundLoaderRef(new StubSoundLoader()));
    setting->SetMaterialLoader(Effekseer::MaterialLoaderRef(new StubMaterialLoader()));
    setting->SetCurveLoader(Effekseer::CurveLoaderRef(new StubCurveLoader()));

    Effekseer::EffectRef effect = Effekseer::Effect::Create(setting, data.data(), int32_t(data.size()));
    if (effect == nullptr)
    {
        fprintf(stderr, "error: not a loadable .efkefc (bad magic/version/corrupt)\n");
        return 1;
    }

    Unsupported unsup;
    NfxDoc doc = extract(effect.Get(), unsup);
    effect = nullptr; // 引用计数释放

    if (!unsup.empty())
    {
        fprintf(stderr, "exit 2: unsupported modules (%zu):\n", unsup.reasons.size());
        for (const auto& r : unsup.reasons)
            fprintf(stderr, "  - %s\n", r.c_str());
        return 2;
    }

    std::string sourceName = inPath.filename().string();
    json j = nfxToJson(doc, sourceName);
    fs::create_directories(outPath.parent_path().empty() ? fs::path(".") : outPath.parent_path());
    FILE* f = fopen(outPath.string().c_str(), "wb");
    if (!f)
    {
        fprintf(stderr, "error: cannot write '%s'\n", outPath.string().c_str());
        return 1;
    }
    std::string text = j.dump(2);
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);

    printf("converted: %s -> %s\n", sourceName.c_str(), outPath.string().c_str());
    printf("  efkVersion : %d\n", doc.efkVersion);
    printf("  rate       : [%.2f, %.2f] /s\n", doc.rateMin, doc.rateMax);
    printf("  life       : [%.2f, %.2f] s\n", doc.lifeMin, doc.lifeMax);
    printf("  spawn      : (%.2f, %.2f, %.2f)\n", doc.spawnPoint[0], doc.spawnPoint[1], doc.spawnPoint[2]);
    printf("  velocity   : box [%.2f..%.2f, %.2f..%.2f, %.2f..%.2f]\n", doc.velMin[0], doc.velMax[0], doc.velMin[1],
           doc.velMax[1], doc.velMin[2], doc.velMax[2]);
    printf("  accel      : (%.2f, %.2f, %.2f)\n", doc.accel[0], doc.accel[1], doc.accel[2]);
    printf("  blend/size : %s / (%.2f, %.2f)\n", doc.blend.c_str(), doc.sizeX, doc.sizeY);
    if (doc.rotEnabled)
        printf("  rotation   : ang [%.4f, %.4f] rad, angVel [%.4f, %.4f] rad/s\n",
               doc.angMin, doc.angMax, doc.angVelMin, doc.angVelMax);
    if (doc.sizeYRangeEnabled)
        printf("  sizeYRange : [%.4f, %.4f] m (per-particle)\n",
               doc.sizeYRangeMin, doc.sizeYRangeMax);
    printf("  fade       : in %.2fs out %.2fs\n", doc.fadeIn, doc.fadeOut);
    printf("  texture    : %s\n", doc.texture.empty() ? "(none)" : doc.texture.c_str());
    return 0;
}

int runSelftest()
{
    // 自证 1：.nfx JSON 往返（write → parse → 字段逐个比对）
    NfxDoc src;
    src.efkVersion = 1810;
    src.rateMin = 300.0f;
    src.rateMax = 300.0f;
    src.lifeMin = 1.6f;
    src.lifeMax = 2.3f;
    for (int a = 0; a < 3; a++)
    {
        src.spawnPoint[a] = 0.1f * float(a);
        src.velMin[a] = -1.5f + float(a);
        src.velMax[a] = 2.5f + float(a);
        src.accel[a] = (a == 1) ? -9.8f : 0.0f;
    }
    src.blend = "additive";
    src.colorStart[0] = 250;
    src.colorStart[1] = 200;
    src.colorStart[2] = 60;
    src.colorEnd[0] = 255;
    src.colorEnd[1] = 90;
    src.colorEnd[2] = 30;
    src.sizeX = 0.08f;
    src.sizeY = 0.08f;
    src.fadeIn = 0.15f;
    src.fadeOut = 0.4f;
    src.texture = "tex/particle01.png";
    // M-PT3：flipbook 字段往返
    src.flipbook.enabled = true;
    src.flipbook.frameLength = 3.0f;
    src.flipbook.countX = 4;
    src.flipbook.countY = 2;
    src.flipbook.loopType = 1;
    src.flipbook.offsetU = 0.0f;
    src.flipbook.offsetV = 0.5f;
    src.flipbook.cellW = 0.25f;
    src.flipbook.cellH = 0.5f;
    // M-PT4 E2：colorCurve 往返（三 RGB 键 + 两 alpha 键，0..255 单位）
    src.colorCurve.enabled = true;
    src.colorCurve.colorKeys = {
        {0.0f, 255.0f, 90.0f, 40.0f},
        {0.5f, 60.0f, 200.0f, 255.0f},
        {1.0f, 255.0f, 255.0f, 255.0f},
    };
    src.colorCurve.alphaKeys = {
        {0.0f, 0.0f},
        {1.0f, 255.0f},
    };
    // M-PT5 E5/R1：旋转 + sizeYRange 往返
    src.rotEnabled = true;
    src.angMin = 0.0f;
    src.angMax = 1.5707964f;    // π/2
    src.angVelMin = -3.1415927f;
    src.angVelMax = 3.1415927f;
    src.sizeYRangeEnabled = true;
    src.sizeYRangeMin = 0.4f;
    src.sizeYRangeMax = 1.2f;

    json j = nfxToJson(src, "roundtrip.efkefc");
    NfxDoc back;
    std::string err;
    if (!nfxFromJson(json::parse(j.dump()), back, err))
    {
        printf("nfx selftest: FAIL (json roundtrip parse: %s)\n", err.c_str());
        return 1;
    }
    bool ok = back.efkVersion == 1810 && back.rateMin == 300.0f && back.rateMax == 300.0f &&
              back.lifeMin == 1.6f && back.lifeMax == 2.3f && back.blend == "additive" &&
              back.colorStart[0] == 250 && back.colorEnd[2] == 30 && back.sizeX == 0.08f &&
              back.fadeIn == 0.15f && back.fadeOut == 0.4f && back.texture == "tex/particle01.png" &&
              back.accel[1] == -9.8f && back.accel[0] == 0.0f && back.velMin[2] == 0.5f &&
              // M-PT3：flipbook 往返
              back.flipbook.enabled && back.flipbook.frameLength == 3.0f &&
              back.flipbook.countX == 4 && back.flipbook.countY == 2 &&
              back.flipbook.loopType == 1 && back.flipbook.offsetV == 0.5f &&
              back.flipbook.cellW == 0.25f && back.flipbook.cellH == 0.5f &&
              // M-PT4 E2：colorCurve 往返
              back.colorCurve.enabled && back.colorCurve.colorKeys.size() == 3 &&
              back.colorCurve.alphaKeys.size() == 2 &&
              back.colorCurve.colorKeys[1] ==
                  std::array<float, 4>{0.5f, 60.0f, 200.0f, 255.0f} &&
              back.colorCurve.alphaKeys[1] == std::array<float, 2>{1.0f, 255.0f} &&
              // M-PT5 E5/R1：旋转 + sizeYRange 往返（π/2 等常量往返逐位）
              back.rotEnabled && back.angMax == 1.5707964f &&
              back.angVelMin == -3.1415927f && back.angVelMax == 3.1415927f &&
              back.sizeYRangeEnabled && back.sizeYRangeMin == 0.4f &&
              back.sizeYRangeMax == 1.2f;
    if (!ok)
    {
        printf("nfx selftest: FAIL (roundtrip field mismatch)\n");
        return 1;
    }

    // 自证 2：坏 magic 拒绝
    json bad = j;
    bad["magic"] = "NFX9";
    if (nfxFromJson(json::parse(bad.dump()), back, err))
    {
        printf("nfx selftest: FAIL (bad magic accepted)\n");
        return 1;
    }

    // 自证 2b：老格式兼容（无 flipbook/colorCurve/ang/sizeYRange 字段 → 全 false）
    {
        json legacy = nfxToJson(src, "legacy.efkefc");
        legacy["render"].erase("flipbook");
        legacy["render"].erase("colorCurve");
        legacy["render"].erase("sizeYRange");
        legacy["emitter"].erase("ang");
        legacy["emitter"].erase("angVel");
        NfxDoc back2;
        std::string err2;
        if (!nfxFromJson(json::parse(legacy.dump()), back2, err2) || back2.flipbook.enabled ||
            back2.colorCurve.enabled || back2.rotEnabled || back2.sizeYRangeEnabled)
        {
            printf("nfx selftest: FAIL (legacy no-optfield parse: %s)\n",
                   err2.empty() ? "optional field enabled" : err2.c_str());
            return 1;
        }
    }

    // 自证 2c-rot：ang/angVel/sizeYRange 畸形拒绝
    {
        auto expectRejectRot = [](const char* tag, const char* key, json patch) {
            json bad = nfxToJson(NfxDoc{}, "rot.efkefc");
            if (key[0] == 's' && key[1] == 'i') // sizeYRange → render
                bad["render"][key] = patch;
            else
                bad["emitter"][key] = patch;
            NfxDoc back3;
            std::string err3;
            if (nfxFromJson(json::parse(bad.dump()), back3, err3))
            {
                printf("nfx selftest: FAIL (%s accepted)\n", tag);
                return false;
            }
            return true;
        };
        bool pass =
            expectRejectRot("ang alone", "ang", json::array({0.0f, 1.0f})) &&
            expectRejectRot("angVel short", "angVel", json::array({0.0f})) &&
            expectRejectRot("ang bad type", "ang", json("not-array")) &&
            expectRejectRot("sizeYRange short", "sizeYRange", json::array({0.5f})) &&
            expectRejectRot("sizeYRange bad type", "sizeYRange", json(42));
        if (!pass)
            return 1;
    }

    // 自证 2d：colorCurve 畸形拒绝（t 越界 / t 非降序 / 键长不对 / 空数组）
    {
        auto expectReject = [](const char* tag, json ccPatch) {
            json bad = nfxToJson(NfxDoc{}, "cc.efkefc");
            bad["render"]["colorCurve"] = ccPatch;
            NfxDoc back3;
            std::string err3;
            if (nfxFromJson(json::parse(bad.dump()), back3, err3))
            {
                printf("nfx selftest: FAIL (colorCurve %s accepted)\n", tag);
                return false;
            }
            return true;
        };
        bool pass =
            expectReject("t out of range", json{{"colorKeys", json::array({json::array({-0.5f, 0, 0, 0})}),
                                                 {"alphaKeys", json::array({json::array({0.0f, 255})})}}}) &&
            expectReject("t unsorted", json{{"colorKeys", json::array({json::array({0.8f, 0, 0, 0}),
                                                                      json::array({0.2f, 0, 0, 0})})},
                                            {"alphaKeys", json::array({json::array({0.0f, 255})})}}) &&
            expectReject("short key", json{{"colorKeys", json::array({json::array({0.0f, 0, 0})})},
                                           {"alphaKeys", json::array({json::array({0.0f, 255})})}}) &&
            expectReject("empty alphaKeys", json{{"colorKeys", json::array({json::array({0.0f, 0, 0, 0})})},
                                                 {"alphaKeys", json::array()}});
        if (!pass)
            return 1;
    }

    // 自证 3：SKFE 魔数识别（拿真加载路径前面的工厂检查逻辑自证）——
    // EfkEfcFile 直接探针：非 SKFE 数据必须判无效
    {
        const char fake[] = "NOTSKFE----";
        Effekseer::EfkEfcFile fakeFile(fake, sizeof(fake) - 1);
        if (fakeFile.IsValid())
        {
            printf("nfx selftest: FAIL (non-SKFE data accepted by EfkEfcFile)\n");
            return 1;
        }
    }

    printf("nfx selftest: PASS (json roundtrip + bad magic + legacy compat + rot/sizeYRange/colorCurve malformed + SKFE probe)\n");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        usageFail("missing command");
    if (strcmp(argv[1], "selftest") == 0 && argc == 2)
        return runSelftest();
    if (strcmp(argv[1], "convert") == 0 && argc == 4)
        return runConvert(argv[2], argv[3]);
    usageFail("unknown command or wrong arity");
}
