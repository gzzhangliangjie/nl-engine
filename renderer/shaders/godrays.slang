// M-NL1：屏幕空间 God Rays（GPU Gems 3 "Volumetric Light Scattering as a
// Post-Process" 口径；拍板 Q-NL3 A 方案，docs/nl/NL-PLAN.md §M-NL1）。
//
// 三段式（applyGodRays 编排，scene.cpp）：
//   1. 遮挡预渲：occlusionMask_（半分辨率 R8_UNORM）清黑 → 光源 quad 渲白
//      （vsGodraysQuad/psGodraysQuad，深度 = 光源 NDC z，Always+写深）→
//      场景几何以 vsShadow+psGodraysBlack 渲黑（深度测试 LESS + 不写深，
//      只盖住「光源前」的遮挡体；vsShadow 复用 model.slang，输入仅 SV_Position）。
//      不用深度派生（深度只有最近表面，光源被挡但几何在其前的场景会错——
//      草案 §M-NL1 拍板口径）。
//   2. radial blur：psGodraysRadial 以光源屏幕 UV 为中心径向采样累积
//      （32 步，density/decay/exposure 三参数 + weight 由 tonemap 复合端控制）。
//   3. 叠加：psTonemap 复合（rt.slang，t3 = godraysAccum_ RGBA16F，
//      g_gr.x = weight，0 时 IEEE 恒等保 bytes）。
//
// Registers（set 0，rt.slang 同款 Vulkan shift 由 CMake 统一旗标处理）：
//   b0 = GodraysConstants（volatile CB，每帧 writeBuffer 全量 64B）
//   t0 = occlusionMask_ SRV（radial blur 读；quad/occl pass 不绑纹理）
//   s0 = 线性 clamp sampler（radial 专用）
// 单光源 = 场景最强点光源（CPU 侧投影进 CB；多光源为后续项，草案口径）。

cbuffer GodraysConstants : register(b0)
{
    // {weight, density, decay, exposure}：weight 在 tonemap 复合端生效（g_gr.x），
    // 此处仅记账；density = 每步采样步长系数，decay = 逐步衰减，exposure = 总强度。
    float4 g_grA;
    // {lightUv.x, lightUv.y, radNdcX, radNdcY}：光源屏幕 UV（vsRtPresent 同款
    // 约定 uv=(ndc.x*0.5+0.5, 0.5-ndc.y*0.5)）+ 光源 quad 半宽/半高（NDC）。
    float4 g_grB;
    // {colorR, colorG, colorB, lightNdcX}：点光颜色（radial 着色）+ 光源 NDC x。
    float4 g_grC;
    // {lightNdcY, lightNdcZ, 0, 0}：光源 NDC y/z（quad 深度 = NDC z，与 vsShadow
    // 的 SV_Position.z 同约定，深度测试双后端一致）。
    float4 g_grD;
}

Texture2D g_maskTex : register(t0);        // occlusionMask_（R8_UNORM 半分辨率）
SamplerState g_grSampler : register(s0);   // 线性 clamp（radial 采样）

// ---- 遮挡预渲：场景几何渲染黑（与 vsShadow 配对，输入签名仅 SV_Position）。
// 不读任何纹理/常量（几何 pass 的 b0/t5 槽位由 shadowBindingLayout_/batch.shadowSet
// 提供，vsShadow 消费；本 PS 零静态引用 → 布局只须覆盖 VS 侧）。
float4 psGodraysBlack(float4 pos : SV_Position) : SV_Target
{
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

// ---- 光源 quad：屏幕空间 NDC 广告牌（4 顶点 triangle-strip，无 VB）。
// 中心 = 光源 NDC xy，半宽/半高 = g_grB.zw，深度 = 光源 NDC z（写深，供
// 几何 pass 的 LESS 测试区分「光源前/后」遮挡）。
struct GrQuadOut
{
    float4 pos   : SV_Position;
    float2 corner: TEXCOORD0;   // 单位角坐标 [-1,1]²（圆盘 discard 用）
};

void vsGodraysQuad(uint vid : SV_VertexID, out GrQuadOut o)
{
    // 0:(-1,-1) 1:(1,-1) 2:(-1,1) 3:(1,1)（vsRtPresent 同款角序）
    float2 c = float2((vid == 1 || vid == 3) ? 1.0f : -1.0f,
                      (vid >= 2) ? 1.0f : -1.0f);
    // g_grC.w = lightNdcX、g_grD.x = lightNdcY、g_grD.y = lightNdcZ
    float2 ndc = float2(g_grC.w, g_grD.x) + c * g_grB.zw;
    o.pos = float4(ndc, g_grD.y, 1.0f);
    o.corner = c;
}

float4 psGodraysQuad(GrQuadOut input) : SV_Target
{
    // 圆盘发射体（单位角坐标内切圆）——方形边缘会在 radial 后留方形痕迹
    //（侧视实测），discard 外接角 = 圆形灯珠。
    if (dot(input.corner, input.corner) > 1.0f) discard;
    return float4(1.0f, 1.0f, 1.0f, 1.0f);   // R8_UNORM 只存 R 通道 = 1.0
}

// ---- 全屏 quad（radial blur 用，vsRtPresent 逐字同款角序 + UV 约定）。
struct GrFullOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

void vsGodraysFull(uint vid : SV_VertexID, out GrFullOut o)
{
    float2 p = float2((vid == 1 || vid == 3) ? 1.0f : -1.0f,
                      (vid >= 2) ? 1.0f : -1.0f);
    o.pos = float4(p, 0.0f, 1.0f);
    o.uv  = float2(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f);
}

// ---- radial blur：32 步向光源 UV 累积（步长/衰减/曝光 = GPU Gems 3 参数式）。
// mask 是 R8_UNORM 单通道遮挡标量（采样返回 (v,0,0,1)——G/B 恒 0，M-NL1 首测
// 教训：读 .rgb = 永远红晕）→ 读 .r 乘 g_grC.rgb 光源颜色着色。输出 RGBA16F。
float4 psGodraysRadial(GrFullOut i) : SV_Target
{
    const int kSamples = 32;
    float2 delta = (g_grB.xy - i.uv) * (g_grA.y / float(kSamples));
    float2 coord = i.uv;
    float  illum = 1.0f;
    float  acc   = 0.0f;
    [unroll] for (int s = 0; s < kSamples; ++s) {
        coord += delta;
        acc += g_maskTex.SampleLevel(g_grSampler, coord, 0.0f).r * illum;
        illum *= g_grA.z;
    }
    return float4(acc * g_grA.w * g_grC.rgb, 1.0f);
}
