// M-UI0：RmlUi NVRHI 共享渲染后端实现。上游契约 = Include/RmlUi/Core/
// RenderInterface.h（必选集）+ SystemInterface.h + FileInterface.h；思路对照
// 官方 GL3 后端的「几何编译 + 逐 draw 状态」结构（抄思路不抄代码——Backends/
// 不入库），落点是本仓 Hud 的 NVRHI 纪律：volatile CB、writeBuffer 上传、
// 每 draw scissor/绑定切换。几何走上游 CompileGeometry 模型（逐几何 VB/IB 常驻，
// ReleaseGeometry 回收），不用 Hud 的共享大缓冲摊平——同一几何多位置渲染时
// 上游以 translation 参数复用，摊平模型表达不了这个复用。
//
// 颜色口径（RmlUi 6.x）：Vertex::colour 与 GenerateTexture 像素都是
// **预乘 alpha**（RGBA 字节序）→ 顶点展开后走 One / InvSrcAlpha 混合（与 Hud 的
// SrcAlpha/InvSrcAlpha 不同，这是预乘标准，不是笔误）。LoadTexture 的 stb 直通
// alpha 需在 CPU 侧转预乘。
//
// 命令列表纪律：CompileGeometry / GenerateTexture / LoadTexture 的上传走独立的
// 一次性 command list，且全部发生在 beginFrame 段（recordFrame 的 frameCmd_ 仍
// 是「帧内唯一 open/close/execute 落点」——M8.2 t0 结构防线不受影响）。
//
// 能力边界（计划 §1）：filters/layers/shaders/clip mask 不实现（上游默认 no-op），
// box-shadow/backdrop-filter 类特效静默降级；transform 恒恒等（SetTransform 不覆写）。

#include "rmlui_backend.hpp"

#include "backend.hpp"                 // readFileBytes（shader 字节码）
#include "nl/core/log.hpp"
#include "nl/core/paths.hpp"
#include "nl/script/Bindings.hpp"      // uiEventFn()（Ui 事件转发落点）

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nl::renderer::detail {
namespace {

// GLFW 键码常量（本文件不 include GLFW 头——renderer 只依赖 NVRHI；数值与
// glfw3.h 对齐：LEFT_SHIFT=340..RIGHT_MENU=348、F1=290..F12=301）。
inline constexpr int kGlfwLeftShift = 340;
inline constexpr int kGlfwRightMenu = 348;

inline float channel255(std::uint8_t v) {
    return static_cast<float>(v) / 255.0f;
}

RmlUiConstants rmlProjectionImpl(float displayWidth, float displayHeight, float tx, float ty) {
    RmlUiConstants out{};
    if (displayWidth <= 0.0f || displayHeight <= 0.0f) {
        out.projection[0] = out.projection[1] = 0.0f;
        out.projection[2] = -1.0f;
        out.projection[3] = 1.0f;
        out.translation[0] = tx;
        out.translation[1] = ty;
        return out;
    }
    // 与 Hud::hudProjection 同公式：ndc.x = x*2/W - 1, ndc.y = -(y*2/H - 1)。
    // translation 在 VS 里先加到像素坐标（同一几何可多位置渲染）。
    out.projection[0] = 2.0f / displayWidth;
    out.projection[1] = -2.0f / displayHeight;
    out.projection[2] = -1.0f;
    out.projection[3] = 1.0f;
    out.translation[0] = tx;
    out.translation[1] = ty;
    return out;
}

// ---- GLFW 键码 -> Rml::Input::KeyIdentifier（demo 需要的子集；未命中回 KI_UNKNOWN）----

Rml::Input::KeyIdentifier glfwToRmlKey(int key) {
    using K = Rml::Input::KeyIdentifier;
    switch (key) {
        case 32:  return K::KI_SPACE;
        case 256: return K::KI_ESCAPE;
        case 257: return K::KI_RETURN;
        case 258: return K::KI_TAB;
        case 259: return K::KI_BACK;
        case 260: return K::KI_INSERT;
        case 261: return K::KI_DELETE;
        case 262: return K::KI_RIGHT;
        case 263: return K::KI_LEFT;
        case 264: return K::KI_DOWN;
        case 265: return K::KI_UP;
        case 266: return K::KI_PRIOR;
        case 267: return K::KI_NEXT;
        case 268: return K::KI_HOME;
        case 269: return K::KI_END;
        case 290: return K::KI_F1;  case 291: return K::KI_F2;
        case 292: return K::KI_F3;  case 293: return K::KI_F4;
        case 294: return K::KI_F5;  case 295: return K::KI_F6;
        case 296: return K::KI_F7;  case 297: return K::KI_F8;
        case 298: return K::KI_F9;  case 299: return K::KI_F10;
        case 300: return K::KI_F11; case 301: return K::KI_F12;
        default: break;
    }
    if (key >= '0' && key <= '9')
        return static_cast<K>(static_cast<int>(K::KI_0) + (key - '0'));
    if (key >= 'A' && key <= 'Z')
        return static_cast<K>(static_cast<int>(K::KI_A) + (key - 'A'));
    if (key >= kGlfwLeftShift && key <= kGlfwRightMenu)
        return K::KI_UNKNOWN;   // 修饰键不当作普通键下发（状态走 modifier 位）
    return K::KI_UNKNOWN;
}

inline int rmlMods(bool ctrl, bool shift, bool alt) {
    return (ctrl ? Rml::Input::KM_CTRL : 0) | (shift ? Rml::Input::KM_SHIFT : 0) |
           (alt ? Rml::Input::KM_ALT : 0);
}

// ---- Ui 事件转发（C# reverse P/Invoke；C++ 侧不缓存 fn，每次现取——热重载兜底）----

class UiEventListener final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override {
        auto fn = nl::script::uiEventFn();
        if (fn == nullptr) return;
        Rml::Element* el = event.GetCurrentElement();
        std::string id = el ? el->GetId() : std::string();
        std::string param;
        if (el != nullptr && el->HasAttribute("onclick"))
            param = el->GetAttribute<Rml::String>("onclick", Rml::String());
        // rc 暂记日志不参与 UI 流程（M-UI0 语义）。
        fn(id.c_str(), event.GetType().c_str(), param.c_str());
    }
};

// 递归挂事件监听：凡带 onclick 属性的元素都挂 "click"（demo 文档 + 脚本加载文档同路）。
void attachEventListeners(Rml::Element* root, UiEventListener& listener) {
    if (root == nullptr) return;
    if (root->HasAttribute("onclick"))
        root->AddEventListener("click", &listener);
    for (int i = 0; i < root->GetNumChildren(); ++i)
        attachEventListeners(root->GetChild(i), listener);
}

} // namespace

// ---- Impl：RmlUi 三接口 + NVRHI 资源 ----

struct RmlUiBackend::Impl : Rml::RenderInterface, Rml::SystemInterface, Rml::FileInterface {
    nvrhi::IDevice* device = nullptr;

    nvrhi::ShaderHandle        vs, ps;
    nvrhi::SamplerHandle       sampler;
    nvrhi::BufferHandle        constants;
    nvrhi::InputLayoutHandle   inputLayout;
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::GraphicsPipelineHandle pipeline;

    // ---- RenderInterface 状态（上游 CompileGeometry 模型）----
    struct Geom {
        nvrhi::BufferHandle vb, ib;
        std::uint32_t indexCount = 0;
    };
    struct Tex {
        nvrhi::TextureHandle tex;
        nvrhi::BindingSetHandle set;
    };
    struct DrawItem {
        std::uintptr_t geom = 0;
        std::uintptr_t tex  = 0;   // 0 = 无纹理（白像素兜底绑定）
        float tx = 0.0f, ty = 0.0f;
        bool  scissorEnabled = false;
        int   sc[4] = {0, 0, 0, 0};   // x0 y0 x1 y1（像素）
    };

    std::unordered_map<std::uintptr_t, Geom> geometries;
    std::unordered_map<std::uintptr_t, Tex>  textures;
    std::uintptr_t nextGeom = 1;
    std::uintptr_t nextTex  = 1;
    std::vector<DrawItem> drawItems;

    bool scissorEnabled = false;
    Rml::Rectanglei scissorRect{};

    // 无纹理几何的兜底：1x1 白纹理（预乘 alpha = 255）。
    Tex whiteTex;

    float displayW = 0.0f, displayH = 0.0f;   // 最近一次 build 的 display 尺寸

    // ---- RmlUi 世界 ----
    Rml::Context* context = nullptr;
    UiEventListener eventListener;
    struct DocRec {
        Rml::ElementDocument* doc = nullptr;   // Close 后置 null（tombstone）
        std::filesystem::path path;            // 热重载 stat 用
        std::filesystem::file_time_type mtime{};
    };
    std::vector<DocRec> documents;             // handle = 下标 + 1
    int demoDocHandle = 0;                     // 0 = 无

    // 输入边沿检测（viewer 每帧喂状态，这里转 press/release 事件）
    bool prevMouseLeft = false;
    bool prevMouseRight = false;

    // ---- FileInterface 状态 ----
    struct FileRec {
        std::ifstream stream;
        std::size_t length = 0;
    };
    std::vector<std::unique_ptr<FileRec>> openFiles;   // FileHandle = 下标 + 1

    bool initShaders(const char* shaderSuffix, const char* vsEntry, const char* psEntry,
                     std::string& outError) {
        auto shaderPath = [shaderSuffix](const char* stage) {
            return nl::core::paths::shaderDir() + "/rmlui." + stage + "." + shaderSuffix;
        };
        std::vector<std::uint8_t> vsBlob, psBlob;
        if (!readFileBytes(shaderPath("vs").c_str(), vsBlob) ||
            !readFileBytes(shaderPath("ps").c_str(), psBlob)) {
            outError = std::string("failed to load rmlui shader (") + shaderSuffix +
                       ") from " + nl::core::paths::shaderDir();
            return false;
        }
        nvrhi::ShaderDesc vsDesc;
        vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName(vsEntry);
        nvrhi::ShaderDesc psDesc;
        psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName(psEntry);
        vs = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
        ps = device->createShader(psDesc, psBlob.data(), psBlob.size());
        if (!vs || !ps) { outError = "createShader failed for rmlui shaders"; return false; }

        nvrhi::BufferDesc cbDesc;
        cbDesc.byteSize = sizeof(RmlUiConstants);
        cbDesc.isConstantBuffer = true;
        cbDesc.isVolatile = true;
        cbDesc.maxVersions = 128;   // 逐 draw 写（同一帧多次），不像 Hud 每帧一次
        cbDesc.debugName = "RmlUiConstants";
        constants = device->createBuffer(cbDesc);
        if (!constants) { outError = "createBuffer failed for RmlUi constants"; return false; }

        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        sampler = device->createSampler(samplerDesc);
        if (!sampler) { outError = "createSampler failed for RmlUi"; return false; }

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0))
            .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        bindingLayout = device->createBindingLayout(layoutDesc);
        if (!bindingLayout) { outError = "createBindingLayout failed for RmlUi"; return false; }

        nvrhi::VertexAttributeDesc attrs[3] = {
            nvrhi::VertexAttributeDesc().setName("POSITION")
                .setFormat(nvrhi::Format::RG32_FLOAT).setOffset(0)
                .setBufferIndex(0).setElementStride(sizeof(RmlUiVertex)),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD")
                .setFormat(nvrhi::Format::RG32_FLOAT).setOffset(8)
                .setBufferIndex(0).setElementStride(sizeof(RmlUiVertex)),
            nvrhi::VertexAttributeDesc().setName("COLOR")
                .setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(16)
                .setBufferIndex(0).setElementStride(sizeof(RmlUiVertex)),
        };
        inputLayout = device->createInputLayout(attrs, 3, vs);
        if (!inputLayout) { outError = "createInputLayout failed for RmlUi"; return false; }

        // 1x1 白像素兜底纹理（texture==0 的无纹理几何用它渲染为纯色面）。
        const std::uint8_t white[4] = {255, 255, 255, 255};
        whiteTex.tex = createRmlTexture(white, 1, 1);
        if (!whiteTex.tex) { outError = "createTexture failed for RmlUi white fallback"; return false; }
        whiteTex.set = bindingSetFor(whiteTex.tex);
        if (!whiteTex.set) { outError = "createBindingSet failed for RmlUi white fallback"; return false; }
        return true;
    }

    nvrhi::TextureHandle createRmlTexture(const void* pixels, int w, int h) {
        nvrhi::TextureDesc desc;
        desc.width = static_cast<std::uint32_t>(w);
        desc.height = static_cast<std::uint32_t>(h);
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.debugName = "RmlUiTexture";
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        nvrhi::TextureHandle tex = device->createTexture(desc);
        if (!tex) return nullptr;
        nvrhi::CommandListHandle upload = device->createCommandList();
        upload->open();
        upload->writeTexture(tex, 0, 0, pixels, static_cast<std::size_t>(w) * 4u);
        upload->close();
        device->executeCommandList(upload);
        return tex;
    }

    nvrhi::BindingSetHandle bindingSetFor(const nvrhi::TextureHandle& tex) {
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, tex))
               .addItem(nvrhi::BindingSetItem::Sampler(0, sampler))
               .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants));
        return device->createBindingSet(setDesc, bindingLayout);
    }

    // ---- SystemInterface ----
    double GetElapsedTime() override {
        using clock = std::chrono::steady_clock;
        static const clock::time_point t0 = clock::now();
        return std::chrono::duration<double>(clock::now() - t0).count();
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        using LT = Rml::Log::Type;
        switch (type) {
            case LT::LT_ERROR:   nl::core::log::errorCh("rmlui", message.c_str()); break;
            case LT::LT_WARNING: nl::core::log::warnCh("rmlui", message.c_str()); break;
            default:             nl::core::log::infoCh("rmlui", message.c_str()); break;
        }
        return true;
    }

    // ---- FileInterface（assets/ 双层解析；Open 句柄 = 下标 + 1）----
    std::filesystem::path resolve(const Rml::String& p) const {
        namespace fs = std::filesystem;
        std::string s(p);
        if (!s.empty() && s.front() == '/') s.erase(s.begin());   // RmlUi 根路径约定
        const fs::path base = fs::path(nl::core::paths::assetDir());
        fs::path cand = base / s;
        std::error_code ec;
        if (fs::exists(cand, ec)) return cand;
        if (fs::exists(s, ec)) return fs::path(s);                // 绝对路径 / cwd 相对
        return cand;   // 最可能候选（Open 失败，RmlUi 日志可见）
    }

    Rml::FileHandle Open(const Rml::String& path) override {
        auto rec = std::make_unique<FileRec>();
        rec->stream.open(resolve(path), std::ios::binary);
        if (!rec->stream.is_open()) return 0;
        rec->stream.seekg(0, std::ios::end);
        rec->length = static_cast<std::size_t>(rec->stream.tellg());
        rec->stream.seekg(0, std::ios::beg);
        openFiles.push_back(std::move(rec));
        return static_cast<Rml::FileHandle>(openFiles.size());
    }
    void Close(Rml::FileHandle file) override {
        const std::size_t idx = static_cast<std::size_t>(file) - 1;
        if (idx < openFiles.size() && openFiles[idx]) {
            openFiles[idx]->stream.close();
            openFiles[idx].reset();
        }
    }
    std::size_t Read(void* buffer, std::size_t size, Rml::FileHandle file) override {
        const std::size_t idx = static_cast<std::size_t>(file) - 1;
        if (idx >= openFiles.size() || !openFiles[idx]) return 0;
        auto& s = openFiles[idx]->stream;
        s.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(s.gcount());
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        const std::size_t idx = static_cast<std::size_t>(file) - 1;
        if (idx >= openFiles.size() || !openFiles[idx]) return false;
        auto& s = openFiles[idx]->stream;
        s.clear();
        s.seekg(offset, static_cast<std::ios_base::seekdir>(origin));
        return static_cast<bool>(s);
    }
    std::size_t Tell(Rml::FileHandle file) override {
        const std::size_t idx = static_cast<std::size_t>(file) - 1;
        if (idx >= openFiles.size() || !openFiles[idx]) return 0;
        return static_cast<std::size_t>(openFiles[idx]->stream.tellg());
    }
    std::size_t Length(Rml::FileHandle file) override {
        const std::size_t idx = static_cast<std::size_t>(file) - 1;
        if (idx >= openFiles.size() || !openFiles[idx]) return 0;
        return openFiles[idx]->length;
    }

    // ---- RenderInterface：纹理 ----
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override {
        const std::size_t need = static_cast<std::size_t>(source_dimensions.x) *
                                 static_cast<std::size_t>(source_dimensions.y) * 4u;
        if (source.size() < need) return 0;
        // 字体 atlas 等内存纹理：像素已是预乘 RGBA（上游契约），直接上传。
        nvrhi::TextureHandle tex = createRmlTexture(source.data(),
                                                    source_dimensions.x, source_dimensions.y);
        if (!tex) return 0;
        Tex t{tex, bindingSetFor(tex)};
        if (!t.set) return 0;
        const std::uintptr_t h = nextTex++;
        textures.emplace(h, std::move(t));
        return static_cast<Rml::TextureHandle>(h);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override {
        const std::filesystem::path full = resolve(source);
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = stbi_load(full.string().c_str(), &w, &h, &comp, 4);
        if (pixels == nullptr || w <= 0 || h <= 0) {
            nl::core::log::warnCh("rmlui", "LoadTexture failed: ", full.string());
            if (pixels) stbi_image_free(pixels);
            return 0;
        }
        // stb 直通 alpha -> 预乘（RmlUi 6.x 契约；GenerateTexture 同口径）。
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t a = pixels[i * 4 + 3];
            pixels[i * 4 + 0] = static_cast<std::uint8_t>(pixels[i * 4 + 0] * a / 255u);
            pixels[i * 4 + 1] = static_cast<std::uint8_t>(pixels[i * 4 + 1] * a / 255u);
            pixels[i * 4 + 2] = static_cast<std::uint8_t>(pixels[i * 4 + 2] * a / 255u);
        }
        nvrhi::TextureHandle tex = createRmlTexture(pixels, w, h);
        stbi_image_free(pixels);
        if (!tex) return 0;
        Tex t{tex, bindingSetFor(tex)};
        if (!t.set) return 0;
        texture_dimensions = Rml::Vector2i(w, h);
        const std::uintptr_t handle = nextTex++;
        textures.emplace(handle, std::move(t));
        return static_cast<Rml::TextureHandle>(handle);
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        const auto it = textures.find(static_cast<std::uintptr_t>(texture));
        if (it == textures.end()) return;
        it->second.set = nullptr;
        it->second.tex = nullptr;
        textures.erase(it);
    }

    // ---- RenderInterface：几何 ----
    Rml::CompiledGeometryHandle CompileGeometry(
            Rml::Span<const Rml::Vertex> verts, Rml::Span<const int> idxs) override {
        if (verts.empty() || idxs.empty()) return 0;
        std::vector<RmlUiVertex> expanded;
        expanded.reserve(verts.size());
        for (const Rml::Vertex& s : verts) {
            RmlUiVertex v{};
            v.x = s.position.x;  v.y = s.position.y;
            v.u = s.tex_coord.x; v.v = s.tex_coord.y;
            // ColourbPremultiplied：RGBA 字节序、预乘（本文件头注释口径）。
            v.r = channel255(s.colour.red);
            v.g = channel255(s.colour.green);
            v.b = channel255(s.colour.blue);
            v.a = channel255(s.colour.alpha);
            expanded.push_back(v);
        }
        std::vector<std::uint32_t> indices;
        indices.reserve(idxs.size());
        for (int i : idxs) indices.push_back(static_cast<std::uint32_t>(i));

        Geom g;
        g.indexCount = static_cast<std::uint32_t>(idxs.size());
        nvrhi::BufferDesc vbDesc;
        vbDesc.byteSize = expanded.size() * sizeof(RmlUiVertex);
        vbDesc.isVertexBuffer = true;
        vbDesc.debugName = "RmlUiGeomVB";
        vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
        vbDesc.keepInitialState = true;
        g.vb = device->createBuffer(vbDesc);
        nvrhi::BufferDesc ibDesc;
        ibDesc.byteSize = indices.size() * sizeof(std::uint32_t);
        ibDesc.isIndexBuffer = true;
        ibDesc.debugName = "RmlUiGeomIB";
        ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
        ibDesc.keepInitialState = true;
        g.ib = device->createBuffer(ibDesc);
        if (!g.vb || !g.ib) return 0;

        nvrhi::CommandListHandle upload = device->createCommandList();
        upload->open();
        upload->writeBuffer(g.vb, expanded.data(), expanded.size() * sizeof(RmlUiVertex));
        upload->writeBuffer(g.ib, indices.data(), indices.size() * sizeof(std::uint32_t));
        upload->close();
        device->executeCommandList(upload);

        const std::uintptr_t handle = nextGeom++;
        geometries.emplace(handle, std::move(g));
        return static_cast<Rml::CompiledGeometryHandle>(handle);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        DrawItem item{};
        item.geom = static_cast<std::uintptr_t>(geometry);
        item.tex  = static_cast<std::uintptr_t>(texture);
        item.tx = translation.x;
        item.ty = translation.y;
        item.scissorEnabled = scissorEnabled;
        const Rml::Vector2i tl = scissorRect.TopLeft();
        const Rml::Vector2i br = scissorRect.BottomRight();
        item.sc[0] = tl.x; item.sc[1] = tl.y; item.sc[2] = br.x; item.sc[3] = br.y;
        drawItems.push_back(item);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
        const auto it = geometries.find(static_cast<std::uintptr_t>(geometry));
        if (it == geometries.end()) return;
        it->second.vb = nullptr;
        it->second.ib = nullptr;
        geometries.erase(it);
    }

    // ---- RenderInterface：scissor（窗口坐标像素）----
    void EnableScissorRegion(bool enable) override { scissorEnabled = enable; }
    void SetScissorRegion(Rml::Rectanglei region) override { scissorRect = region; }

    void resetRenderState() {
        geometries.clear();
        textures.clear();
        whiteTex.tex = nullptr;
        whiteTex.set = nullptr;
        drawItems.clear();
        nextGeom = nextTex = 1;
        scissorEnabled = false;
        scissorRect = Rml::Rectanglei::MakeInvalid();
    }
};

// ---- RmlUiBackend 转发实现 ----

RmlUiConstants rmlProjection(float displayWidth, float displayHeight, float tx, float ty) {
    return rmlProjectionImpl(displayWidth, displayHeight, tx, ty);
}

bool RmlUiBackend::init(nvrhi::IDevice* device, const char* shaderSuffix,
                        const char* vsEntry, const char* psEntry, std::string& outError) {
    shutdown();
    auto impl = std::make_unique<Impl>();
    impl->device = device;

    // NVRHI 侧资源（shader/CB/sampler/layout/白像素兜底）。
    if (!impl->initShaders(shaderSuffix, vsEntry, psEntry, outError)) {
        outError = "RmlUiBackend: " + outError;
        return false;
    }

    // RmlUi 三接口注册 + 世界初始化（System/File/Render 同一 Impl）。
    Rml::SetSystemInterface(impl.get());
    Rml::SetFileInterface(impl.get());
    Rml::SetRenderInterface(impl.get());
    Rml::Initialise();

    const int w = 1280, h = 720;   // 初始尺寸；首帧 build() 按实际 display 尺寸 SetDimensions
    impl->context = Rml::CreateContext("main", Rml::Vector2i(w, h));
    if (impl->context == nullptr) {
        outError = "RmlUiBackend: CreateContext failed";
        Rml::Shutdown();
        return false;
    }

    // 字体：RmlUi 无字体加载不了任何文档（M-UI0.0 硬前提）。
    const std::string fontPath =
        (std::filesystem::path(nl::core::paths::assetDir()) / "ui/fonts/NotoSans-Regular.ttf").string();
    if (!Rml::LoadFontFace(fontPath)) {
        outError = "RmlUiBackend: LoadFontFace failed: " + fontPath;
        Rml::Shutdown();
        return false;
    }

    // 调试器插件（F8 翻转可见性；backend 只在 --rmlui-demo 下创建，基线零扰动）。
    Rml::Debugger::Initialise(impl->context);

    // demo 文档（loadDocument 复用同一通道：handle、事件挂接、mtime 快照全一致）。
    impl_ = impl.release();
    const int demo = loadDocument("ui/demo.rml");
    if (demo <= 0) {
        outError = "RmlUiBackend: failed to load ui/demo.rml";
        shutdown();
        return false;
    }
    impl_->demoDocHandle = demo;

    nl::core::log::infoCh("renderer", "RmlUi ready: context=main | font=NotoSans-Regular | demo doc handle ", demo);
    return true;
}

void RmlUiBackend::shutdown() {
    if (impl_ == nullptr) return;
    Impl* impl = impl_;
    impl_ = nullptr;
    if (impl->context != nullptr) {
        Rml::Debugger::Shutdown();
        Rml::RemoveContext("main");
        impl->context = nullptr;
    }
    Rml::Shutdown();   // 文档/几何/纹理引用先随 context 释放，再清 NVRHI 侧账本
    impl->resetRenderState();
    impl->documents.clear();
    impl->demoDocHandle = 0;
    impl->pipeline = nullptr;
    impl->bindingLayout = nullptr;
    impl->inputLayout = nullptr;
    impl->constants = nullptr;
    impl->sampler = nullptr;
    impl->ps = nullptr; impl->vs = nullptr;
    impl->device = nullptr;
    delete impl;
}

bool RmlUiBackend::ensurePipeline(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                                  std::string& outError) {
    if (impl_ == nullptr) { outError = "RmlUiBackend not initialised"; return false; }
    Impl& impl = *impl_;
    if (impl.pipeline) return true;
    nvrhi::GraphicsPipelineDesc desc;
    desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setInputLayout(impl.inputLayout)
        .setVertexShader(impl.vs)
        .setFragmentShader(impl.ps)
        .addBindingLayout(impl.bindingLayout);
    desc.renderState.rasterState
        .setCullNone()
        .setFrontCounterClockwise(true)
        .enableScissor();
    desc.renderState.depthStencilState
        .disableDepthTest().disableDepthWrite();
    // 预乘 alpha：src = One / dst = InvSrcAlpha（含 alpha 通道；上游 GL3 后端同口径）。
    auto& rt = desc.renderState.blendState.targets[0];
    rt.enableBlend()
      .setSrcBlend(nvrhi::BlendFactor::One)
      .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOp(nvrhi::BlendOp::Add)
      .setSrcBlendAlpha(nvrhi::BlendFactor::One)
      .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOpAlpha(nvrhi::BlendOp::Add);
    impl.pipeline = device->createGraphicsPipeline(desc, info);
    if (!impl.pipeline) { outError = "createGraphicsPipeline failed for RmlUi"; return false; }
    return true;
}

void RmlUiBackend::invalidatePipeline() {
    if (impl_ != nullptr) impl_->pipeline = nullptr;
}

void RmlUiBackend::build(RmlUiFrame& frame) {
    if (impl_ == nullptr || !frame.enabled) return;
    Impl& impl = *impl_;

    Rml::Context* ctx = impl.context;
    if (ctx == nullptr) return;

    // 尺寸变化（resize 通道：viewer 每帧喂 display 尺寸）。
    const int w = static_cast<int>(frame.displayWidth);
    const int h = static_cast<int>(frame.displayHeight);
    if (w > 0 && h > 0) {
        const Rml::Vector2i cur = ctx->GetDimensions();
        if (cur.x != w || cur.y != h) ctx->SetDimensions(Rml::Vector2i(w, h));
        impl.displayW = frame.displayWidth;
        impl.displayH = frame.displayHeight;
    }

    // ---- 输入喂数 ----
    ctx->ProcessMouseMove(static_cast<int>(frame.mouseX), static_cast<int>(frame.mouseY), 0);
    if (frame.mouseLeftDown != impl.prevMouseLeft) {
        if (frame.mouseLeftDown) ctx->ProcessMouseButtonDown(0, 0);
        else                     ctx->ProcessMouseButtonUp(0, 0);
        impl.prevMouseLeft = frame.mouseLeftDown;
    }
    if (frame.mouseRightDown != impl.prevMouseRight) {
        if (frame.mouseRightDown) ctx->ProcessMouseButtonDown(1, 0);
        else                      ctx->ProcessMouseButtonUp(1, 0);
        impl.prevMouseRight = frame.mouseRightDown;
    }
    // GLFW scroll 向上为正；RmlUi wheel_delta 正值 = 向右/向下（上游 Context.h 契约）。
    if (frame.mouseWheel != 0.0f) ctx->ProcessMouseWheel(-frame.mouseWheel, 0);

    for (int i = 0; i < frame.keyEventCount && i < 16; ++i) {
        const auto& ke = frame.keyEvents[i];
        // F8 = RmlUi 调试器翻转（M-UI0 开发者体验；backend 只在 demo 模式存在，
        // 不与既有热键冲突）。
        if (ke.key == 297 /*GLFW_KEY_F8*/ && ke.down) {
            Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
            continue;
        }
        const Rml::Input::KeyIdentifier ki = glfwToRmlKey(ke.key);
        if (ki == Rml::Input::KI_UNKNOWN) continue;
        const int mods = rmlMods(ke.ctrl, ke.shift, ke.alt);
        if (ke.down) ctx->ProcessKeyDown(ki, mods);
        else         ctx->ProcessKeyUp(ki, mods);
    }
    for (int i = 0; i < frame.textCharCount && i < 32; ++i)
        ctx->ProcessTextInput(static_cast<Rml::Character>(frame.textChars[i]));

    // ---- 热重载（demo 文档 mtime 驱动；无编辑器的体验补偿）----
    if (impl.demoDocHandle > 0) {
        Impl::DocRec& demo = impl.documents[static_cast<std::size_t>(impl.demoDocHandle) - 1];
        if (demo.doc != nullptr && !demo.path.empty()) {
            std::error_code ec;
            const auto cur = std::filesystem::last_write_time(demo.path, ec);
            if (!ec && demo.mtime != std::filesystem::file_time_type{} && cur != demo.mtime) {
                nl::core::log::infoCh("rmlui", "hot reload: ", demo.path.string());
                // 复用 loadDocument 通道重建（事件挂接 + mtime 快照全一致）；旧 handle
                // 变 tombstone，脚本若持有旧 handle 得 kUiInvalid = 失效安全拒绝。
                const int fresh = loadDocument("ui/demo.rml");
                if (fresh > 0) impl.demoDocHandle = fresh;
            } else if (!ec) {
                demo.mtime = cur;
            }
        }
    }

    // ---- Update + Render（RenderInterface 收集 draw items）----
    impl.drawItems.clear();
    ctx->Update();
    ctx->Render();
}

void RmlUiBackend::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                        const nvrhi::Viewport& viewport) {
    if (impl_ == nullptr || impl_->drawItems.empty() || !impl_->pipeline || !framebuffer)
        return;
    Impl& impl = *impl_;

    for (const Impl::DrawItem& item : impl.drawItems) {
        const auto git = impl.geometries.find(item.geom);
        if (git == impl.geometries.end() || !git->second.vb || !git->second.ib) continue;
        const Impl::Geom& geom = git->second;

        const Impl::Tex* tex = nullptr;
        if (item.tex != 0) {
            const auto tit = impl.textures.find(item.tex);
            if (tit != impl.textures.end()) tex = &tit->second;
        }
        if (tex == nullptr) tex = &impl.whiteTex;   // 无纹理几何兜底

        const RmlUiConstants cb =
            rmlProjectionImpl(impl.displayW, impl.displayH, item.tx, item.ty);
        cmd->writeBuffer(impl.constants, &cb, sizeof(cb));

        nvrhi::GraphicsState state;
        state.setPipeline(impl.pipeline)
             .setFramebuffer(framebuffer)
             .addBindingSet(tex->set)
             .addVertexBuffer(nvrhi::VertexBufferBinding()
                 .setBuffer(geom.vb).setSlot(0).setOffset(0))
             .setIndexBuffer(nvrhi::IndexBufferBinding()
                 .setBuffer(geom.ib).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
        // Rect 构造参数顺序是 (minX, maxX, minY, maxY)（M0.5 教训，别按 xyxy 写）。
        // scissor 关闭时全屏（与 RmlUi 语义一致：EnableScissorRegion(false) = 不裁剪）。
        if (item.scissorEnabled) {
            const int x0 = std::clamp(item.sc[0], 0, static_cast<int>(viewport.maxX));
            const int y0 = std::clamp(item.sc[1], 0, static_cast<int>(viewport.maxY));
            const int x1 = std::clamp(item.sc[2], x0, static_cast<int>(viewport.maxX));
            const int y1 = std::clamp(item.sc[3], y0, static_cast<int>(viewport.maxY));
            if (x1 > x0 && y1 > y0)
                state.viewport.addScissorRect(nvrhi::Rect(x0, x1, y0, y1));
        }
        state.viewport.addViewport(viewport);
        cmd->setGraphicsState(state);
        nvrhi::DrawArguments args;
        args.setVertexCount(geom.indexCount);
        cmd->drawIndexed(args);
    }
}

// ---- UiSink 落点实现（Renderer::rml* 转发进来；Bindings.cpp 是唯一调用方）----

int RmlUiBackend::loadDocument(const char* path) {
    if (impl_ == nullptr || path == nullptr || path[0] == '\0') return 0;
    Impl& impl = *impl_;
    Rml::ElementDocument* doc = impl.context->LoadDocument(path);
    if (doc == nullptr) return 0;
    attachEventListeners(doc, impl.eventListener);
    doc->Show();
    Impl::DocRec rec;
    rec.doc = doc;
    rec.path = impl.resolve(path);
    std::error_code ec;
    rec.mtime = std::filesystem::last_write_time(rec.path, ec);
    impl.documents.push_back(std::move(rec));
    return static_cast<int>(impl.documents.size());
}

bool RmlUiBackend::closeDocument(int handle) {
    if (impl_ == nullptr || handle <= 0 ||
        static_cast<std::size_t>(handle) > impl_->documents.size())
        return false;
    Impl::DocRec& rec = impl_->documents[static_cast<std::size_t>(handle) - 1];
    if (rec.doc == nullptr) return false;   // tombstone：已关闭，失效安全拒绝
    if (impl_->demoDocHandle == handle) impl_->demoDocHandle = 0;
    rec.doc->Close();   // 实际析构推迟到下一次 Update()（上游契约）
    rec.doc = nullptr;
    return true;
}

bool RmlUiBackend::showDocument(int handle, bool show) {
    if (impl_ == nullptr || handle <= 0 ||
        static_cast<std::size_t>(handle) > impl_->documents.size())
        return false;
    Impl::DocRec& rec = impl_->documents[static_cast<std::size_t>(handle) - 1];
    if (rec.doc == nullptr) return false;
    if (show) rec.doc->Show();
    else      rec.doc->Hide();
    return true;
}

bool RmlUiBackend::elementSetText(int handle, const char* elId, const char* text) {
    if (impl_ == nullptr || handle <= 0 || elId == nullptr || text == nullptr ||
        static_cast<std::size_t>(handle) > impl_->documents.size())
        return false;
    Impl::DocRec& rec = impl_->documents[static_cast<std::size_t>(handle) - 1];
    if (rec.doc == nullptr) return false;
    Rml::Element* el = rec.doc->GetElementById(elId);
    if (el == nullptr) return false;
    el->SetInnerRML(text);
    return true;
}

bool RmlUiBackend::elementGetText(int handle, const char* elId, const char** outText) {
    if (impl_ == nullptr || handle <= 0 || elId == nullptr || outText == nullptr ||
        static_cast<std::size_t>(handle) > impl_->documents.size())
        return false;
    Impl::DocRec& rec = impl_->documents[static_cast<std::size_t>(handle) - 1];
    if (rec.doc == nullptr) return false;
    Rml::Element* el = rec.doc->GetElementById(elId);
    if (el == nullptr) return false;
    // 静态缓冲「下次同线程调用前有效」账本（Bindings.hpp 口径；C# 立即拷贝）。
    static std::string buffer;
    buffer = el->GetInnerRML();
    *outText = buffer.c_str();
    return true;
}

} // namespace nl::renderer::detail
