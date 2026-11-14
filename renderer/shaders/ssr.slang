// M-WAT0（water 轨首里程碑）：SSR 屏幕空间反射本体（UE SSRT 口径，静态相机
// 确定性演示版）。任务书 docs/WATER-PLAN.md §9b 落码口径（全部有明文先例）：
//
//   1) march 对齐 UE SSRT（Shaders/Private/SSRT/SSRTRayCast.ush）：UVz 三元组
//      线性插值 march（Stachowiak 屏幕空间射线口径，UE 同款近似）、容差带
//      hit、bUncertain、mip 按粗糙度、屏幕外 miss、法线偏移防自命中。
//   2) 金字塔口径：**径向距离（线性）min 金字塔**。度量复用 S6 hiz.slang 的
//      视点径向距离（SSR-RESEARCH-NOTES §3：同像素视线单调，容差用世界空间
//      距离表达）；但**算子与 S6 相反**——S6 是 max（遮挡剔除语义：「簇最近点
//      vs 区域最远表面」），SSR 跳步要的是 min（最近表面）：rayZ < 区域最近
//      距离 → 射线在区域内一切表面之前 → 整格跳过（Stachowiak Hi-Z skip）；
//      rayZ ≥ 最近距离 → 可能已穿越 → 降 mip 细化。因此 ssr.slang 自建
//      ssrDistPyramid_，不复用 hizPyramid_（算子/构建链/消费模式三重不同，
//      且 scene 模式本就不建 S6 金字塔）。
//   3) 深度来源 = vis buffer 的 deviceZ（visUnpack 反解）→ invMvp 反投影世界
//      坐标 → 径向距离。Q4 冻结不新增属性纹理：法线/粗糙度 vis buffer 即取
//      即算（gbuffer_common.slangh + 材质间接表，与 direct.slang 同构绑定；
//      S16-PLAN §58 明文口径）。
//   4) 场景色模糊链 ssrSceneMip_：RGBA32F 均值 2x2（SSR-RESEARCH-NOTES §3.1
//      「颜色是平均语义」；NVRHI 无双后端统一 generateMips → 自写均值下采样，
//      兼得双后端字节确定性）。链 POT 化（与金字塔同尺寸）→ 奇数边界不出现，
//      clamp 读双保险。链 mip0 = 半分辨率均值（2px 起模糊，注释口径）。
//   5) 合成：march 输出 ssrColor_（RGBA32F，rgb=反射辐射/a=权重）→
//      csSSRCompose 加权叠加写回 hdrSceneColor_（UAV 全替换先例 = cache viz；
//      RMW 同纹理先例 = csGatherCompose）。插点 = T0 链 tonemap 之前。
//   6) 确定性：固定步数 / 固定 maxDist / 无 jitter / 无时域累积——同场景同
//      相机双后端 bytes 逐位一致（demo 判据）。miss → sky.slangh 解析天穹
//      （现有公共函数，不触碰 sky.slangh 一行——Q-WA5 预案不触发）。
//
// Resource registers（§2.7 每管线单一 BindingSet；槽位分配避开 resolve 表
// t0..t12/s0 与 b0..b2，全文件零寄存器复用声明——hiz.slang 双 u0 同 register
// 异型声明虽有余例，本文件一律独占槽位更稳）：
//   csSSRDepth   : b4 SsrDepthConstants(96B) / t0 vis / u0 金字塔 mip0
//   csSSRDown    : b5 SsrDownConstants(16B) / t16 金字塔单 mip / u1 下一级（min 2x2）
//   csSSRMip     : b5 SsrDownConstants(16B) / t17 场景色链单 mip / u2 下一级（均值 2x2）
//   csSSR(march) : b0/b1/b2 + t0..t12 + s0 = resolve 槽位表（addResolveLayoutItems
//                  同源；b0/b1 被绑不读）+ b3 SsrConstants(208B) + t13 金字塔
//                  全 mip + t14 场景色链全 mip + u0 ssrColor_
//   csSSRCompose : b5 SsrDownConstants(16B) / t18 ssrColor_ / u3 hdrSceneColor_
// SPIR-V 绑定平移与 direct.slang 同：b→256, t→0, s→128, u→384。
// DXIL SM6.6：仅 csSSR（NonUniformResourceIndex 选 mrTex 槽，同 direct.slang）。

#include "visbuf_common.slangh"
#include "gbuffer_common.slangh"
#include "sky.slangh"
#include "water_common.slangh"  // M-WAT1：水体解析法线 + WaterConstants 布局规范

// M-WAT1（拍板 A）：水体常量（b6，288B；ssr 侧 b1 空置、b2=b0 别名防碰不用，
// 取 b6）。--water 关时渲染端不建不绑本 CB。
cbuffer WaterConstants : register(b6)
{
    float4 g_wcA[8];   // (D.x, D.y, A, k) × 8 波
    float4 g_wcB[8];   // (omega, Q, 0, 0) × 8 波
    float4 g_wcC;      // (time, baseColor.rgb)
    float4 g_wcD;      // (ambient, specPow, specGain, maxAmp)
};

// ============================================================================
// 常量块
// ============================================================================

// csSSRDepth 专用（b4，96B volatile）：invMvp 反投影 + eye + 视口尺寸。
cbuffer SsrDepthConstants : register(b4)
{
    float4x4 g_invMvp;      // inverseMat4(params.mvp)（M8.2/probe 同款 CPU 求逆）
    float4   g_eyeSize;     // xyz = eye 世界坐标，w 预留
    float4   g_dims;        // x = width, y = height, zw 预留
}

// csSSRDown / csSSRMip / csSSRCompose 共用形制（b5，16B volatile，逐 dispatch
// 覆写——hiz 逐级常量同款）。
cbuffer SsrDownConstants : register(b5)
{
    float4 g_down;          // (srcW, srcH, tgtW, tgtH)；compose 用 (w, h, 0, 0)
}

// csSSR march 专用（b3，208B volatile）。
cbuffer SsrConstants : register(b3)
{
    float4x4 g_fwdViewProj; // params.viewProj（march 世界点投影回屏幕；纯 view×proj）
    float4x4 g_invMvpM;     // inverseMat4(params.mvp)（本入口不读；与 depth 反解
                            // 同源登记，调试采样预留）
    float4   g_eyePos;      // xyz = eye 世界坐标, w = maxDist（march 世界距离，米）
    float4   g_march;       // x = 步数, y = 容差带 band(米), z = mipScale, w = 金字塔 mip 数
    float4   g_chain;       // x = 场景色链 mip 数, yzw 预留
    float4   g_ssrZenith;   // rgb = 天顶色（FrameParams.weatherZenith 同源）
    float4   g_ssrHorizon;  // rgb = 地平线色（FrameParams.weatherHorizon 同源）
}

// march 读 resolve 表 b0（g_mvp/g_model 反解几何，material.slang 同款）与
// b2（宽高）。b1 被绑不读（NVRHI 多绑合法）。
cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model（vis 光栅同矩阵）
    float4x4 g_model;      // model 矩阵（世界空间变换）
    float4   g_lightDir;
    float4   g_options;
}

cbuffer MatResolveConstants : register(b2)
{
    float g_width;
    float g_height;
    float2 _pad;
}

// ---- 资源 ----
StructuredBuffer<uint64_t> g_visSRV       : register(t0);   // depth / march
Texture2D<float>           g_pyrSrc       : register(t16);  // down：单 mip 子资源
Texture2D<float4>          g_chainSrc     : register(t17);  // mip：单 mip 子资源
Texture2D<float>           g_ssrPyramid   : register(t13);  // march：全 mip（Load 绝对 mip 号）
Texture2D<float4>          g_ssrChain     : register(t14);  // march：场景色链全 mip
Texture2D<float4>          g_ssrInput     : register(t18);  // compose：ssrColor_
StructuredBuffer<float>    g_vertexPool   : register(t1);   // march
StructuredBuffer<uint>     g_localIndices : register(t2);   // march
StructuredBuffer<uint>     g_clusterMeta  : register(t3);   // march
Texture2D<float4>          g_mrTex[kMaxVisMaterials] : register(t8);   // march
StructuredBuffer<GPUMaterial> g_materialTable : register(t12);        // march
SamplerState               g_sampler      : register(s0);   // march

// ---- 输出 ----
RWTexture2D<float>  g_pyramidMip0 : register(u0);   // csSSRDepth
RWTexture2D<float>  g_pyramidDst  : register(u1);   // csSSRDown
RWTexture2D<float4> g_chainDst    : register(u2);   // csSSRMip
RWTexture2D<float4> g_ssrOut      : register(u0);   // csSSR
RWTexture2D<float4> g_hdrCompose  : register(u3);   // csSSRCompose

// ============================================================================
// 1) csSSRDepth：vis buffer → 径向距离 mip0。
//    空像素（天空/背景）= 1e9（min 金字塔下最远者不参与「最近」，不会让跳步
//    越过真实几何；视口外区域同理——POT 化 mip0 静区清 1e9）。
// ============================================================================
[shader("compute")]
[numthreads(8, 8, 1)]
void csSSRDepth(uint3 tid : SV_DispatchThreadID)
{
    const uint width  = uint(g_dims.x);
    const uint height = uint(g_dims.y);
    const uint2 pixel = tid.xy;
    if (pixel.x >= width || pixel.y >= height) return;

    const uint64_t v = g_visSRV[visIndex(pixel, width)];
    if (v == VIS_EMPTY) {
        g_pyramidMip0[pixel] = 1.0e9f;
        return;
    }
    uint clusterId, triId;
    float deviceZ;
    visUnpack(v, clusterId, triId, deviceZ);

    // deviceZ → 世界坐标 → 视点径向距离（S6 hiz 度量口径同源）。
    const float2 size = float2(g_dims.x, g_dims.y);
    const float3 worldPos = gbReconstructWorldPos(pixel, deviceZ, size, g_invMvp);
    const float d = distance(worldPos, g_eyeSize.xyz);
    g_pyramidMip0[pixel] = d;
}

// ============================================================================
// 2) csSSRDown：径向距离 **min** 2x2 降采样（结构照 hiz.slang csHizDown，
//    算子 max→min。语义差异见文件头 §2：min 才支持 Hi-Z 整格跳步；clamp 读
//    重复同一 texel 不改变真值 min，保守方向正确）。
//    SRV 绑单 mip 子资源（Load LOD 相对视图基 mip），UAV 绑下一级。
// ============================================================================
[shader("compute")]
[numthreads(8, 8, 1)]
void csSSRDown(uint3 tid : SV_DispatchThreadID)
{
    const uint2 tgt = uint2(g_down.z, g_down.w);
    if (tid.x >= tgt.x || tid.y >= tgt.y) return;

    const int  srcW = int(g_down.x), srcH = int(g_down.y);
    const int2 base = int2(2u * tid.x, 2u * tid.y);
    const int2 lo   = int2(min(base.x,     srcW - 1), min(base.y,     srcH - 1));
    const int2 hi   = int2(min(base.x + 1, srcW - 1), min(base.y + 1, srcH - 1));

    float v = g_pyrSrc.Load(int3(lo.x, lo.y, 0));
    v = min(v, g_pyrSrc.Load(int3(hi.x, lo.y, 0)));
    v = min(v, g_pyrSrc.Load(int3(lo.x, hi.y, 0)));
    v = min(v, g_pyrSrc.Load(int3(hi.x, hi.y, 0)));
    g_pyramidDst[uint2(tid.x, tid.y)] = v;
}

// ============================================================================
// 3) csSSRMip：场景色均值 2x2 降采样（SSR-RESEARCH-NOTES §3.1「颜色是平均
//    语义」；generateMips 双后端无统一入口 → 手写均值，字节确定性兼得）。
// ============================================================================
[shader("compute")]
[numthreads(8, 8, 1)]
void csSSRMip(uint3 tid : SV_DispatchThreadID)
{
    const uint2 tgt = uint2(g_down.z, g_down.w);
    if (tid.x >= tgt.x || tid.y >= tgt.y) return;

    const int  srcW = int(g_down.x), srcH = int(g_down.y);
    const int2 base = int2(2u * tid.x, 2u * tid.y);
    const int2 lo   = int2(min(base.x,     srcW - 1), min(base.y,     srcH - 1));
    const int2 hi   = int2(min(base.x + 1, srcW - 1), min(base.y + 1, srcH - 1));

    float4 v = g_chainSrc.Load(int3(lo.x, lo.y, 0));
    v += g_chainSrc.Load(int3(hi.x, lo.y, 0));
    v += g_chainSrc.Load(int3(lo.x, hi.y, 0));
    v += g_chainSrc.Load(int3(hi.x, hi.y, 0));
    g_chainDst[uint2(tid.x, tid.y)] = v * 0.25f;
}

// ============================================================================
// 4) csSSR：march 本体（UE SSRT 口径的确定性简化版）。
//    - UVz 线性插值（起点=当前像素表面点，终点=p0 + R·maxDist 投影；uv 用与
//      Load/写 mip0 一致的 y-down 纹理行序，投影侧 y 翻转与 gbuffer_common
//      屏幕映射同式）；
//    - min 金字塔跳步：rayZ < Rmin(mip) → 跳约一格宽（k += 格宽换算步数）；
//      rayZ ≥ Rmin(mip) → 降 mip 细化（同 k 重试，无步进）；
//    - mip0 穿越：rayZ - Rpx ≤ band → 确切 hit；否则 bUncertain（权重减半）；
//    - 步数耗尽 / 射线出屏 / 背相机 → miss → 解析天穹（sky.slangh）。
// ============================================================================
[shader("compute")]
[numthreads(8, 8, 1)]
void csSSR(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    const uint  width  = uint(g_width);
    const uint  height = uint(g_height);
    if (pixel.x >= width || pixel.y >= height) return;

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);   // 缺省零权重（compose 无操作）

    const uint64_t v = g_visSRV[visIndex(pixel, width)];
    if (v != VIS_EMPTY) {
        uint clusterId, triId;
        float deviceZ;
        visUnpack(v, clusterId, triId, deviceZ);

        // 簇元数据 → 材质（roughness 来源；槽 3 = 子网格序号 = materialId）。
        const uint metaBase   = clusterId * 4u;
        const uint vertexBase = g_clusterMeta[metaBase + 0u];
        const uint localBase  = g_clusterMeta[metaBase + 2u];
        const uint materialId = g_clusterMeta[metaBase + 3u];
        // M-WAT1：水体标记守卫（越界禁读；MR 采样照常走槽 0 占位但水体分支
        // 不消费 roughness——解析法线口径下用固定低粗糙度档）。
        const bool isWater = (materialId == NL_WATER_MATERIAL_ID);
        const GPUMaterial mat = isWater ? g_materialTable[0] : g_materialTable[materialId];

        // vertex pulling 三顶点（8 float/顶点）+ vis 反解（S16-PLAN §58 口径）。
        float3 pos[3], nrm[3];
        float2 uv[3];
        for (uint k = 0u; k < 3u; ++k) {
            const uint localIdx = g_localIndices[localBase + triId * 3u + k];
            const uint gv = vertexBase + localIdx;
            const uint b  = gv * 8u;
            pos[k] = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);
            nrm[k] = float3(g_vertexPool[b + 3u], g_vertexPool[b + 4u], g_vertexPool[b + 5u]);
            uv[k]  = float2(g_vertexPool[b + 6u], g_vertexPool[b + 7u]);
        }
        const float2 fragPos = float2(pixel) + 0.5f;
        const float2 size    = float2(width, height);
        const GBufGeometry gbuf = gbInterpolateVisTriangle(
            pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2],
            uv[0], uv[1], uv[2], g_mvp, g_model, fragPos, g_width, g_height);

        // 粗糙度：材质 MR 纹理 g 通道 × 因子（direct.slang 同式；槽号非一致
        // → NonUniformResourceIndex，SM6.6）。水体 = 固定低粗糙度（0.10，
        // 波面 glossy；M-WAT2 折射后随 Jacobian 调制）。
        float roughness = 0.10f;
        if (!isWater) {
            const float4 mrTex = g_mrTex[NonUniformResourceIndex(mat.mrSlot)]
                .SampleGrad(g_sampler, gbuf.uv, gbuf.ddxUv, gbuf.ddyUv);
            roughness = clamp(mrTex.g * mat.mrFactor.y, 0.045f, 1.0f);
        }

        // M-WAT1：水体起点 = 位移后表面 + 解析法线（与 direct.slang 同一
        // waterGerstner；deviceZ 已是 VS 位移后的真实深度，反解无需修正）。
        float3 surfPos = gbuf.worldPos;
        float3 n = normalize(gbuf.normal);
        if (isWater) {
            float3 waterDisp;
            n = waterGerstner(gbuf.worldPos.xz, g_wcC.x, g_wcA, g_wcB, waterDisp);
            surfPos = gbuf.worldPos + waterDisp;
        }
        const float3 V = normalize(g_eyePos.xyz - surfPos);
        const float3 R = reflect(-V, n);

        // march 端点：起点 = 表面点沿法线小偏移（防自命中），终点 = R·maxDist。
        const float3 p0   = surfPos + n * 0.02f;
        const float3 pEnd = p0 + R * g_eyePos.w;
        const float  z0   = distance(p0, g_eyePos.xyz);
        const float  z1   = distance(pEnd, g_eyePos.xyz);

        // UVz 端点（Stachowiak 口径：uv 与 z 都按 k/steps 线性插值）。uv 空间 =
        // y-down 纹理行序（与 csSSRDepth 写 mip0 / Load 采样同一坐标系）。
        float2 uv0 = (float2(pixel) + 0.5f) / size;
        float2 uv1 = uv0;
        bool endValid = false;
        const float4 clipEnd = mul(g_fwdViewProj, float4(pEnd, 1.0f));
        if (clipEnd.w > 1.0e-6f) {
            const float2 ndc1 = clipEnd.xy / clipEnd.w;
            uv1 = float2(ndc1.x * 0.5f + 0.5f, 1.0f - (ndc1.y * 0.5f + 0.5f));
            endValid = true;
        }

        const uint  steps     = uint(g_march.x);
        const float band      = g_march.y;
        const float mipScale  = g_march.z;
        const uint  pyrMips   = uint(g_march.w);
        const uint  chainMips = uint(g_chain.x);
        const uint mipStart  = min(uint(roughness * mipScale), pyrMips - 1u);
        uint  mip = mipStart;
        // 场景色链 mip0 不建（免拷贝 hdr）：链从 mip1（2px 均值）起，march 采样
        // 档位 = mipStart+1（链总 mip 数 = kSsrChainMips+1，clamp 到最细可用层）。
        const uint colorMip  = min(mipStart + 1u, chainMips - 1u);

        // 屏幕射线总跨度（px）：粗 mip 跳步的 k 步进换算用。
        const float spanPx = max(length((uv1 - uv0) * size), 1.0f);

        bool   hit = false;
        bool   uncertain = false;
        float2 hitUv = uv0;
        uint   k = 0u;
        uint   guard = 0u;
        while (k < steps && guard < 256u) {
            ++guard;
            const float f = float(k) / float(steps);
            const float2 uv   = lerp(uv0, uv1, f);
            const float  rayZ = lerp(z0, z1, f);

            // 出屏 / 背相机 → miss（UE 远平面拒绝 + 屏幕边界同口径）。
            if (!endValid) break;
            if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) break;

            const uint2 px = uint2(min(uv * size, size - 1.0f));
            const float rmin = g_ssrPyramid.Load(int3(px, mip));

            if (rayZ >= rmin) {
                if (mip > 0u) { --mip; continue; }   // 细化：同 k 重试更细层
                // mip0 穿越判定（容差带）。
                hit    = true;
                hitUv  = uv;
                uncertain = (rayZ - rmin) > band;
                break;
            }
            // rayZ < rmin：射线在本格一切表面之前 → 跳过约一格宽。
            const uint  cellTexels = 1u << mip;
            const uint  kAdv = max(1u, uint(float(cellTexels * steps) / spanPx));
            k += kAdv;
        }

        if (hit) {
            // 反射色 = 场景色链在命中 uv 的粗糙度档采样。
            const float3 refl = g_ssrChain.SampleLevel(g_sampler, hitUv, float(colorMip)).rgb;
            float w = 1.0f - roughness;
            if (uncertain) w *= 0.5f;
            result = float4(refl, saturate(w));
        } else {
            // miss → 解析天穹（现有公共函数；场景色已 gamma 编码，反射侧同样
            // 编码保持合成口径一致）。
            const float3 sky = skyGradientParams(R, g_ssrZenith.rgb, g_ssrHorizon.rgb);
            const float3 enc = pow(saturate(sky), 1.0f / 2.2f);
            result = float4(enc, saturate((1.0f - roughness) * 0.35f));
        }
    }

    g_ssrOut[pixel] = result;
}

// ============================================================================
// 5) csSSRCompose：反射能量加权叠加回 hdrSceneColor_（tonemap 前；UAV 全替换
//    先例 = cache viz / irrad dump；RMW 同纹理先例 = csGatherCompose）。
//    叠加式（不是替换）：场景色已含 diffuse，反射是镜面能量加项。
// ============================================================================
[shader("compute")]
[numthreads(8, 8, 1)]
void csSSRCompose(uint3 tid : SV_DispatchThreadID)
{
    const uint2 tgt = uint2(g_down.x, g_down.y);
    if (tid.x >= tgt.x || tid.y >= tgt.y) return;
    const float4 ssr = g_ssrInput.Load(int3(int2(tid.xy), 0));
    const float4 hdr = g_hdrCompose[tid.xy];
    g_hdrCompose[tid.xy] = float4(hdr.rgb + ssr.rgb * ssr.a, hdr.a);
}
