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
    j["render"] = {
        {"blend", d.blend},
        {"colorStart", {d.colorStart[0], d.colorStart[1], d.colorStart[2], d.colorStart[3]}},
        {"colorEnd", {d.colorEnd[0], d.colorEnd[1], d.colorEnd[2], d.colorEnd[3]}},
        {"size", {d.sizeX, d.sizeY}},
        {"fadeIn", d.fadeIn},
        {"fadeOut", d.fadeOut},
        {"texture", d.texture.empty() ? json(nullptr) : json(d.texture)},
    };
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

    // 缩放：None/Fixed
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
    else
    {
        unsup.add("scaling type != None/Fixed (PVA/Easing/FCurve)");
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
    for (const auto& uv : rc.UVs)
    {
        if (uv.Type != UVAnimationType::Default)
            unsup.add("UV animation != Default");
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
    else
    {
        unsup.add("color type != Fixed/Random (Easing/FCurve/Gradient)");
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

    // 反例清单：旋转/力场/碰撞/音效/GPU 粒子/KillRules/LOD/Falloff/AlphaCutoff
    if (n.RotationParam.RotationType != ParameterRotationType::ParameterRotationType_None &&
        n.RotationParam.RotationType != ParameterRotationType::ParameterRotationType_Fixed)
        unsup.add("rotation type != None/Fixed (PVA/Easing/FCurve)");
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
              back.accel[1] == -9.8f && back.accel[0] == 0.0f && back.velMin[2] == 0.5f;
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

    printf("nfx selftest: PASS (json roundtrip + bad magic + SKFE probe)\n");
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
