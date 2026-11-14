// M10.2 (S10): Final Gather — 每像素探针插值 + 与直光合成（S10-PLAN M10.2，
// 2026-11-11 Q13-1/Q13-2/Q13-3 拍板全取方案 A）。
//
// csGather（纯采样，零射线）：
//   每像素：probeDepth_ 深度 → 天空早退（Carrot isSky 口径）→ 反投影世界位置
//   → 深度邻域 3-tap 重建法线（Q4 冻结面延续，零新增 GBuffer 资源）→
//   Q13-1 按像素深度分段选级（deviceZ 0.25/0.5/0.75 → L0..L3）→
//   UE CalculateUniformUpsampleInterpolationWeights 口径（2026-11-11 实读
//   LumenScreenProbeGather.usf L147-210 + CVar 默认值锁定）：
//     双线性 corner（BilinearExpand=1）：tile00 = min(pix/stride, grid-2)，
//     bilinear = (pix - tile00·stride + 1) / (stride + 2)
//     平面深度权重 = exp2(-10000·(planeDist/sceneDepth)²)（主）
//                   / exp2(-1000·(planeDist/sceneDepth)²)（Fallback，CVar 0.1×）
//     planeDist = |dot(probePos,N) − dot(wp,N)|（UE ScenePlane 4d 点面距）
//     sceneDepth 用 length(wp−eye) 近似（UE 是线性视深，相对量纲一致）
//     探针深度=0（天空探针）→ 权重 0（UE CornerDepths>0 select 口径）
//   权重和 < MIN(0.01, UE #define) → 换 FallbackWeights 再判 → 仍不足 → 0
//   （UE 1211-1223 归一化口径：W /= max(sum, MIN)）。
//   每 corner 八面体采样：方向 = 像素法线（M10.1 viz 已验证物理合理口径；
//   「探针→眼睛」凸面恒 miss 已被 M10.1 冒烟实锤否掉）。八面体图存的是
//   「弹射表面出射 radiance」（M10.1 trace 已烤进 albedo），gather 输出 =
//   albedo_pixel × 采样值——albedo_pixel 不可得（GBuffer 冻结），用统一暖灰
//   （M9.1 降级族同口径，traceCB g_traceHitMat.rgb 同源）。
//
// csGatherCompose（Q13-2 真替换链路）：
//   直光管线 ambient 已置零（material.slang g_options.z=1 开关，gather 模式才开
//   ——既有用例 bytes 保形）。合成 = 去 producer gamma + 间接光 + 回 gamma：
//   out = pow(saturate(pow(hdr,2.2) + gather), 1/2.2)。hdr 同纹理 SRV 读 +
//   UAV 写（每线程只 RMW 自己的 texel，单线程内自洽；DX12/Vulkan 均合法），
//   免 rtTarget_ 中转——presentTonemap 收尾切 SRV 的现成链路直通。
//
// Registers（§2.7 每管线单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   b0 = GatherConstants(128B volatile) + t0..t3 place atlas + t4..t7 irrad atlas
//        + t8 probeDepth_ + t9 hdrSceneColor_（SRV 读）+ u0 gatherLinear_
//        + u1 hdrSceneColor_（UAV 写，与 t9 同纹理 RMW）
//   两条管线（csGather / csGatherCompose）共用同一 BindingSet。

#include "octahedral.slangh"
#include "gbuffer_common.slangh"
#include "sky.slangh"

cbuffer GatherConstants : register(b0)
{
    float4x4 g_gInvViewProj;   // 本帧 inverseMat4(viewProj)（反投影）
    float4   g_gEye;           // xyz = eye（sceneDepth 欧氏近似）
    float4   g_gParams;        // x = baseRes / y = viewW / z = viewH / w = octRes(8)
    float4   g_gMat;           // rgb = albedo（统一暖灰，traceCB 同源）/ a = MIN 权重(0.01)
    float4   g_gLevels;        // Q13-1 线性深度分段阈值（sceneDepth = length(wp-eye)，
                               //  欧氏单位）：x<t1→L0 / <t2→L1 / <t3→L2 / else L3。
                               //  w = --sky-bg 开关（M10.3 Q14-4：compose 背景像素
                               //  写 skyGradient(viewDir)，与探针 miss 天穹同函数；
                               //  默认 0 = clearColor 保形）。
                               //  ⚠️ 不能用 deviceZ 分段——透视投影非线性，可见场景
                               //  deviceZ 全挤在 [0.9,1] → 全落 L3（2×2 网格），
                               //  M10.2 冒烟实锤（画面整体变暗 = ambient 白拿）。
    // M14C1（S14-C）：[32] = 自适应探针开关（probe-adaptive 模式置 1；CB 扩容
    // 128→144B，[33..35] 保留）。开 = 插值池 4→8（每 corner 并入归属格位的
    // atlas 预留区槽位探针，place/irrad 同映射 (x, res+y)）；关 = 原 4 池逐值
    // 一致（s10-*/s12-* 冻结基线零漂）。
    float4   g_gAdaptive;
}

Texture2D<float4> g_place0  : register(t0);   // 布点 atlas（RGBA32F worldPos+deviceZ）
Texture2D<float4> g_place1  : register(t1);
Texture2D<float4> g_place2  : register(t2);
Texture2D<float4> g_place3  : register(t3);
Texture2D<float4> g_irrad0  : register(t4);   // 辐照度 atlas（RGBA16F，a=1 texel 有效）
Texture2D<float4> g_irrad1  : register(t5);
Texture2D<float4> g_irrad2  : register(t6);
Texture2D<float4> g_irrad3  : register(t7);
Texture2D<float4> g_depth   : register(t8);   // probeDepth_（velocity prepass 深度）
Texture2D<float4> g_hdr     : register(t9);   // 直光 encoded 画面（compose 读）

RWTexture2D<float4> g_gatherOut : register(u0);   // gatherLinear_（RGBA16F 全屏）
RWTexture2D<float4> g_hdrUAV    : register(u1);   // hdrSceneColor_（compose RMW 写）

// place atlas 4 路分支读（纹理数组不能动态索引，probe_viz 同款）。
float4 loadPlaceA(uint level, uint2 texel)
{
    if (level == 0u) return g_place0.Load(int3(texel, 0));
    if (level == 1u) return g_place1.Load(int3(texel, 0));
    if (level == 2u) return g_place2.Load(int3(texel, 0));
    return g_place3.Load(int3(texel, 0));
}

float3 loadIrradA(uint level, uint2 texel)
{
    if (level == 0u) return g_irrad0.Load(int3(texel, 0)).rgb;
    if (level == 1u) return g_irrad1.Load(int3(texel, 0)).rgb;
    if (level == 2u) return g_irrad2.Load(int3(texel, 0)).rgb;
    return g_irrad3.Load(int3(texel, 0)).rgb;
}

// 深度邻域 3-tap 反投影 cross 重建法线（probe.slang probeNormalFromDepth 同款
// 口径：越界 clamp + 朝向相机翻转；纯 compute 无 ddx/ddy）。
bool gatherNormalFromDepth(int2 pix, float3 wp, float2 size, out float3 n)
{
    n = float3(0.0f, 1.0f, 0.0f);
    const int2 maxPix = int2(size) - 1;
    const int2 pr = min(pix + int2(1, 0), maxPix);
    const int2 pu = min(pix + int2(0, 1), maxPix);
    const float zr = g_depth.Load(int3(pr, 0)).x;
    const float zu = g_depth.Load(int3(pu, 0)).x;
    if (zr >= 1.0f || zu >= 1.0f) return false;    // 天空邻域：给默认向上法线
    const float3 wr = gbReconstructWorldPos(uint2(pr), zr, size, g_gInvViewProj);
    const float3 wu = gbReconstructWorldPos(uint2(pu), zu, size, g_gInvViewProj);
    n = normalize(cross(wr - wp, wu - wp));
    // 朝向相机（叉积手性不保证向外）：gather 永远服务可见表面。
    if (dot(n, wp - g_gEye.xyz) > 0.0f) n = -n;
    return true;
}

// Q13-1：像素深度分段选级——**线性深度**（欧氏 sceneDepth）分段，阈值 CB 传入。
// deviceZ 分段已被冒烟实锤否掉（非线性挤压全落 L3，见 g_gLevels 注释）。
uint gatherLevel(float sceneDepth)
{
    if (sceneDepth < g_gLevels.x) return 0u;
    if (sceneDepth < g_gLevels.y) return 1u;
    if (sceneDepth < g_gLevels.z) return 2u;
    return 3u;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csGather(uint3 tid : SV_DispatchThreadID)
{
    const float viewW = g_gParams.y;
    const float viewH = g_gParams.z;
    if (tid.x >= uint(viewW) || tid.y >= uint(viewH)) return;
    const int2  pix  = int2(tid.xy);
    const float2 size = float2(viewW, viewH);

    const float deviceZ = g_depth.Load(int3(pix, 0)).x;
    if (deviceZ <= 0.0f || deviceZ >= 1.0f)
    {
        g_gatherOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);   // 天空：合成加 0
        return;
    }

    const float3 wp = gbReconstructWorldPos(tid.xy, deviceZ, size, g_gInvViewProj);
    float3 n = float3(0.0f, 1.0f, 0.0f);
    gatherNormalFromDepth(pix, wp, size, n);
    const float sceneDepth = length(wp - g_gEye.xyz);   // 线性视深欧氏近似

    const uint  level  = gatherLevel(sceneDepth);
    const uint  res    = uint(g_gParams.x) >> level;   // 本级网格分辨率（atlas 尺寸）
    const uint  octRes = uint(g_gParams.w);
    const float kMin   = g_gMat.a;

    // ⚠️ 探针屏幕格距 = viewport / res_l（csPlace 口径，非 2 的幂）——M10.2 踩坑：
    // 用 baseRes>>level 当 stride 会把 tile 算到 atlas 越界（Load 越界静默返 0
    // → probe.w 全 0 → 权重全灭 → gather 输出全黑）。tile 取连续格坐标 floor，
    // bw = 小数部分（探针格中心 (i+0.5) 布点，像素中心对齐时 bw=0.5）。
    const float2 gridF = float2(tid.xy) * float(res) / float2(g_gParams.y, g_gParams.z);
    const uint2 tile = min(uint2(floor(gridF)), uint2(res, res) - 2u);
    const float2 bw = saturate(gridF - float2(tile));
    const float4 bilinear = float4(
        (1.0f - bw.y) * (1.0f - bw.x),   // 00
        (1.0f - bw.y) * bw.x,            // 10
        bw.y * (1.0f - bw.x),            // 01
        bw.y * bw.x);                    // 11

    const float d2 = dot(wp, n);
    const int2 octT = octaDirToTexel(normalize(n), octRes);

    // M14C1：插值池 4→8（g_gAdaptive.x 开关）。k=0..3 = 均匀 corner；k=4..7 =
    // 同 corner 格位的自适应探针（atlas 预留区槽位 (x, res+y)，Q-C2A 布点形态）。
    // 自适应探针 a=0（空槽/未 spawn）→ 权重 0（UE select 口径）。
    const bool  useAdaptive = g_gAdaptive.x > 0.5f;
    float weights[8];
    float fallback[4];
    float3 irr[8];
    [unroll]
    for (uint k = 0u; k < 8u; ++k) { weights[k] = 0.0f; irr[k] = float3(0.0f, 0.0f, 0.0f); }
    [unroll]
    for (uint k = 0u; k < 4u; ++k) fallback[k] = 0.0f;

    // ---- 均匀 4 角（原口径逐值一致；同时收集角点世界位供 spawn 尺度自标定）----
    float3 uniPos[4];
    bool  uniOk[4];
    [unroll]
    for (uint k = 0u; k < 4u; ++k) { uniPos[k] = float3(0.0f, 0.0f, 0.0f); uniOk[k] = false; }
    [unroll]
    for (uint k = 0u; k < 4u; ++k)
    {
        const int2  off = int2(k & 1u, k >> 1u);   // 00/10/01/11
        const uint2 ct  = tile + uint2(off);
        // M14C1 根修（2026-11-14 拍板转正）：uniform 区越界守卫。adaptive atlas 高 ×2
        //（下半 spawn 预留区），L3 级 res=1 时 (res,res)-2 uint 下溢使 tile 钳位失效，
        // corner01 ct=(0,1) 落预留区槽位——spawn 探针被当 uniform corner 双线性插值
        //（diag19/20 实锤 = adaptive≠u8 的 213.8 全部来源；guard 后 adp↔u8 逐位一致，
        // u8 路径 c20≡d12 逐位不变，L0-2 恒 no-op）。
        if (ct.x >= res || ct.y >= res) continue;
        const float4 probe = loadPlaceA(level, ct);
        if (probe.w <= 0.0f) continue;            // 天空探针：权重 0（UE select 口径）

        uniOk[k] = true;
        uniPos[k] = probe.xyz;
        const float planeDist = abs(dot(probe.xyz, n) - d2);
        const float rel = planeDist / max(sceneDepth, 1e-4f);
        weights[k]  = bilinear[k] * exp2(-10000.0f * rel * rel);   // 主（CVar 1.0）
        // Fallback 权重只对均匀 4 角保留原语义（自适应探针不参与降级链，
        // 与 UE bUseAdaptiveProbes 分支的独立池口径一致）。
        fallback[k] = bilinear[k] * exp2(-1000.0f * rel * rel);    // Fallback（0.1×）
        // 探针格基址 + border 偏移（M10.1 viz 踩坑同款：漏乘基址全黑）。
        irr[k] = loadIrradA(level, ct * (octRes + 2u) + uint2(octT) + 1u);
    }

    // ---- 自适应探针（拍板 A 根修，2026-11-14）：权重 = 距离衰减 × 平面权 ----
    // 初版借 corner 双线性权（探针实际位置在格位内部任意点 ≠ corner）→ 空间错位
    // tile 条带污染（差异热图 + MSE 实锤：spawn 区 214 偏移且远离 u32 参照）。
    // 改按探针真实世界位置距离衰减：w = exp2(-4·(d/spacing)²) × exp2(-10000·rel²)。
    // spacing 自标定 = 相邻均匀角点世界距均值（零新 CB 常量，CB 布局不动）；
    // d=0.5·spacing → 0.5，d=1·spacing → 1/16（主影响域 = 本格位）。非 adaptive
    // 模式此段整体跳过（保形）。
    // M14C2（拍板 A，2026-11-14）：spacing 门降级。自标定门要求「相邻均匀角点对
    // 双侧有效」，但 diag21/22 排查实锤：覆盖不足格位（L2 全空角点 / L3 单探针）
    // 恰是 spawn 的服务对象——门与服务对象结构性互斥，spawn 池全帧 0 次进入。
    // 降级：spacingN==0 时 spacing 改由像素线性视深 × 探针格屏幕角尺寸估算
    // spacing_fb = sceneDepth·(2tan(fovY/2))/res_l（探针格铺满视口，格距 ∝ 深度×
    // 角尺寸；2tan(fovY/2) 由 CPU 侧 g_gAdaptive.y 提供，FrameParams.projYScale
    // 同源）。UE 无此门（自适应探针直接进 upsample 池按世界距衰减）。
    if (useAdaptive)
    {
        float  spacingSum = 0.0f;
        uint   spacingN   = 0u;
        [unroll]
        for (uint k = 0u; k < 4u; ++k)
        {
            // 相邻对：(0,1) (2,3) 横向，(0,2) (1,3) 纵向——双侧有效才算。
            const uint a = (k & 1u) | ((k >> 1u) << 1u);          // 0,1,2,3
            const uint bH = a ^ 1u;                                // 横向邻居
            const uint bV = a ^ 2u;                                // 纵向邻居
            if (uniOk[a] && uniOk[bH] && a < bH)
                { spacingSum += length(uniPos[a] - uniPos[bH]); ++spacingN; }
            if (uniOk[a] && uniOk[bV] && a < bV)
                { spacingSum += length(uniPos[a] - uniPos[bV]); ++spacingN; }
        }
        const float spacing = (spacingN > 0u)
            ? spacingSum / float(spacingN)                         // 自标定（M14C1 原语义）
            : sceneDepth * g_gAdaptive.y / float(res);             // 降级估算（M14C2）
        // g_gAdaptive.z = spawn 池总开关（--probe-adaptive-nospawn 置 0）：
        // 验收门「禁 spawn adaptive ≡ u8 逐位一致」的冻结路径保形仪表。
        if (g_gAdaptive.z > 0.5f && spacing > 1e-4f)
        {
            [loop]
            for (uint k = 4u; k < 8u; ++k)
            {
                const int2  off = int2(k & 1u, (k >> 1u) & 1u);
                const uint2 ct  = tile + uint2(off);
                const uint2 at  = uint2(ct.x, res + ct.y);   // 预留区槽位
                const float4 probe = loadPlaceA(level, at);
                if (probe.w <= 0.0f) continue;               // 空槽/未 spawn
                const float planeDist = abs(dot(probe.xyz, n) - d2);
                const float rel = planeDist / max(sceneDepth, 1e-4f);
                const float d   = length(wp - probe.xyz);
                const float t   = d / spacing;
                weights[k] = exp2(-4.0f * t * t) * exp2(-10000.0f * rel * rel);
                irr[k] = loadIrradA(level, at * (octRes + 2u) + uint2(octT) + 1u);
            }
        }
    }

    // M14C1 收口修正 + M14C2 拍板 A 修订：spawn 候选贡献限幅（UE
    // AdaptiveProbeAllocationFraction=0.5 字面口径）。追加形态 + 共享 corner 角权
    // → 失效格位 spawn 权重池结构性翻倍（实测白斑：剪影 spawn 探针的外向 texel =
    // 天空 miss 亮值，面部像素被对半混入）。限 spawn 总权重 ≤ 均匀 4 角权重和
    // （50% 分配上限）。**M14C2 修订**：限幅仅在 uniSum > 0（混合格位）生效——
    // 均匀角全空的覆盖不足格位（spawn 的服务对象）uniSum=0 时原实现 scl=0 把
    // spawn 一并清零，fallback 降级形同虚设；全空格位 spawn 独立生效（无双重
    // 计权，白斑前提不成立）。非 adaptive 模式 spawn 候选权重恒 0，此段零效果。
    if (useAdaptive)
    {
        float uniSum = 0.0f;
        [unroll]
        for (uint k = 0u; k < 4u; ++k) uniSum += weights[k];
        float spawnSum = 0.0f;
        [unroll]
        for (uint k = 4u; k < 8u; ++k) spawnSum += weights[k];
        if (uniSum > 0.0f && spawnSum > uniSum)
        {
            const float scl = uniSum / spawnSum;
            [unroll]
            for (uint k = 4u; k < 8u; ++k) weights[k] *= scl;
        }
    }

    // UE 两级降级链（1211-1223）：主权重和 < MIN → Fallback；仍 < MIN → 全 0。
    float sum = 0.0f;
    [unroll]
    for (uint k = 0u; k < 8u; ++k) sum += weights[k];
    if (sum < kMin)
    {
        [unroll]
        for (uint k = 0u; k < 8u; ++k) weights[k] = (k < 4u) ? fallback[k] : 0.0f;
        sum = 0.0f;
        [unroll]
        for (uint k = 0u; k < 8u; ++k) sum += weights[k];
    }

    float3 result = float3(0.0f, 0.0f, 0.0f);
    if (sum >= kMin)
    {
        [unroll]
        for (uint k = 0u; k < 8u; ++k)
            result += irr[k] * weights[k];
        result /= max(sum, kMin);
    }

    // 间接出射 = albedo × 入射 radiance（八面体图已烤命中 albedo，本处乘
    // 像素侧 albedo——统一暖灰降级族，traceCB g_traceHitMat.rgb 同源）。
    // 间接出射 = albedo × 入射 radiance（八面体图已烤命中 albedo，本处乘
    // 像素侧 albedo——统一暖灰降级族，traceCB g_traceHitMat.rgb 同源）。
    g_gatherOut[tid.xy] = float4(g_gMat.rgb * result, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csGatherCompose(uint3 tid : SV_DispatchThreadID)
{
    const float viewW = g_gParams.y;
    const float viewH = g_gParams.z;
    if (tid.x >= uint(viewW) || tid.y >= uint(viewH)) return;

    // M10.3（Q14-4）：--sky-bg 开——背景像素（deviceZ>=1 清值）不透 clearColor，
    // 直接写解析天穹（skyGradient 与探针 miss 同函数；viewDir = 远平面反投影
    // worldPos - eye 方向）。前景像素走正常 RMW 合成。
    if (g_gLevels.w > 0.5f)
    {
        const float  bgZ = g_depth.Load(int3(int2(tid.xy), 0)).x;
        if (bgZ >= 1.0f)
        {
            const float3 wpFar = gbReconstructWorldPos(tid.xy, 1.0f, float2(viewW, viewH),
                                                       g_gInvViewProj);
            const float3 vd = normalize(wpFar - g_gEye.xyz);
            g_hdrUAV[tid.xy] = float4(skyGradient(vd), 1.0f);
            return;
        }
    }

    // 直光画面已含 producer gamma（material.slang 输出端 pow 1/2.2）；
    // 间接光是线性域 → 去 gamma 相加后回 gamma。ambient 项已由 g_options.z=1
    // 在直光管线压到 0.03 微底（Q13-2 拍板勘误，GI 为主）。同纹理 SRV 读 +
    // UAV 写，每线程只 RMW 自己的 texel。
    const int2 spix = int2(tid.xy);
    const float4 enc = g_hdr.Load(int3(spix, 0));
    const float4 gi  = g_gatherOut.Load(int2(spix));   // RWTexture2D.Load = int2（SRV 才是 int3）
    const float3 lin    = pow(saturate(enc.rgb), 2.2f);
    const float3 outLin = lin + gi.rgb;
    const float3 outEnc = pow(saturate(outLin), 1.0f / 2.2f);
    g_hdrUAV[tid.xy] = float4(outEnc, enc.a);
}
