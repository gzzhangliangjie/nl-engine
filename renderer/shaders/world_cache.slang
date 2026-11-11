// S11 M11.0 (S11): world cache flat hashmap infrastructure — Q11-0 拍板方案 A
// （docs/s11/S11-PLAN.md，跟 bevy/Carrot 口径：无墓碑，life 衰减到 0 清零立即重用
// + 线性探测上限 3 步 + 查询命中续命；自愈 = cell 按内容 key、下次查询自然重插）。
//
// 三个 compute 入口：
//   csCacheFill   半分辨率（CacheDownscaleFactor=2，Carrot 口径）屏幕采样 →
//                 probeDepth 反投影世界位置 + 3-tap 法线 → 量化 → 查询即插入
//                 （bevy 口径：CAS 空槽占用 / 同 checksum 续命 / 碰撞 +1）。
//   csCacheDecay  全表 life-1，到 0 清 checksum（bevy decay_world_cache 同语义）。
//   csCacheViz    全分辨率每像素 query-only 查表：命中 = hash 伪彩 × 寿命亮度，
//                 miss = 暗底——「cell 分布符合场景」的自检图（M11.0 --mode cache-viz）。
// M11.1 增两入口（cell 更新 = 多跳反馈的机制本体，S11-PLAN M11.1）：
//   csCacheUpdate 每活跃 cell 软目标选中后：1 条对光源 shadow ray（DI 可见性）
//                 + 1 条 cosine 随机方向邻居射线（命中查缓存 × albedo / miss
//                 天穹）→ 新样本写 wcNewRadiance（w=1 标记）。
//   csCacheBlend  只混合标记 cell：lerp(old, new, 1/min(a+1,64))，样本数 cap 64
//                 （bevy blend_new_samples 线性版；luminance-delta 自适应归 M11.3）。
//
// 语义的 CPU 参照实现 = gi-tools/main.cpp WorldCacheSim（先仿真后上 GPU 的计划底线）；
// selftest 断言的插入/回收/碰撞/容量语义在 shader 侧逐条对应。
//
// Registers（§2.7 每管线单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   fill/decay 共用一套 set：b0 CacheConstants + t0 probeDepth SRV
//     + u0 checksums + u1 life + u2 geoPos + u3 geoNrm（decay 只用 u0/u1）
//   viz 独立 set：b0 CacheConstants + t0 probeDepth + t1 checksums + t2 life
//     + u0 hdrSceneColor_（UAV RMW 全替换，同 csGatherCompose 呈现链口径）

#include "gbuffer_common.slangh"
#include "sky.slangh"     // M11.1：邻居射线 miss 天穹（csCacheUpdate GI 段）

// ---- 与 gi-tools WorldCacheSim / scene.cpp 逐字同口径的常量 ----
static const uint kCacheCapacity  = 1u << 18;   // 262144 槽（bevy 2^20 的 1/4，场景尺度小）
static const uint kMaxProbeSteps  = 3;          // Q11-0：bevy 探测上限
static const float kCellSize      = 0.5f;       // 计划缺省（S11-PLAN M11.0）

cbuffer CacheConstants : register(b0)
{
    float4x4 g_invViewProj;   // 列主序（CPU inverseMat4，probe 链同一路径）
    float4   g_eyeCell;       // xyz = eye（保留），w = cellSize（恒 kCellSize，占位可调）
    float4   g_params;        // x=vpW y=vpH z=lifetime w=frameIndex（r2 抖动种子）
    // M11.1 cell 更新（⚠️ SSC §2.21：CPU 布局逐字段对齐，scene.cpp cb[24..39]）
    float4   g_updateLight;   // xyz = 指向光源（M9 g_traceLightDir 同语义）, w = 强度
    float4   g_updateCol;     // rgb = 灯色（白）, w = shadowBias（世界尺度）
    float4   g_updateMat;     // rgb = 统一间接反照率（0.45/0.39/0.30，M10.x 同口径）,
                              // w = shadow tMax
    float4   g_updateCtl;     // x = 软目标概率 p, y = viz 亮度模式（0=life/1=radiance）,
                              // z/w = 保留
}

Texture2D g_probeDepth : register(t0);   // D32（velocity prepass 写入）

// fill set（decay 复用同 set，只碰 u0/u1）
RWStructuredBuffer<uint>   g_checksumsW : register(u0);   // 0 = 空槽
RWStructuredBuffer<uint>   g_lifeW      : register(u1);
RWStructuredBuffer<float4> g_geoPosW    : register(u2);   // xyz = cell 采样世界位置
RWStructuredBuffer<float4> g_geoNrmW    : register(u3);   // xyz = cell 采样法线

// viz / update / blend 共读（M11.1：全入口唯一 register 号，零复用歧义）
StructuredBuffer<uint> g_checksumsR : register(t1);
StructuredBuffer<uint> g_lifeR      : register(t2);
StructuredBuffer<float4> g_geoPosR  : register(t3);   // update 读 cell 几何
StructuredBuffer<float4> g_geoNrmR  : register(t4);
StructuredBuffer<float4> g_radianceR    : register(t5);   // 上帧收敛值 C（update/ viz 亮度）
StructuredBuffer<float4> g_newRadianceR : register(t6);   // blend 读本帧新样本
RaytracingAccelerationStructure g_tlas : register(t7);    // update 的 shadow/GI 射线

RWStructuredBuffer<float4> g_newRadianceW : register(u4); // update 写新样本（w=1 标记）
RWStructuredBuffer<float4> g_radianceW    : register(u5); // blend 写收敛值（rgb=C, a=samples）

// viz set
RWTexture2D<float4>    g_hdrOut     : register(u0);       // hdrSceneColor_ 全替换

// ---- 双哈希（与 gi-tools 逐字同构；pcg/iqint 为公开通用算法）----
uint wcPcg(uint input) {
    const uint state = input * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint wcIqint(uint input) {
    const uint n = (input << 13u) ^ input;
    return n * (n * n * 15731u + 789221u) + 1376312589u;
}

void wcMakeKey(float3 wp, float3 n, out uint key, out uint checksum) {
    const int3 qp = int3(floor(wp / kCellSize));
    const int3 qn = int3(floor(n + 0.0001f));
    uint k = wcPcg(uint(qp.x));
    k = wcPcg(k + uint(qp.y));
    k = wcPcg(k + uint(qp.z));
    k = wcPcg(k + uint(qn.x));
    k = wcPcg(k + uint(qn.y));
    k = wcPcg(k + uint(qn.z));
    key = k & (kCacheCapacity - 1u);
    uint c = wcIqint(uint(qp.x));
    c = wcIqint(c + uint(qp.y));
    c = wcIqint(c + uint(qp.z));
    c = wcIqint(c + uint(qn.x));
    c = wcIqint(c + uint(qn.y));
    c = wcIqint(c + uint(qn.z));
    checksum = max(c, 1u);   // 0 保留为空槽
}

// 深度邻域 3-tap 法线（probe.slang probeNormalFromDepth 同口径；返回 false = 邻域含天空）
bool wcNormalFromDepth(int2 pix, float3 wp, float2 size, out float3 n) {
    n = float3(0.0f, 1.0f, 0.0f);
    const int2 maxPix = int2(size) - 1;
    const int2 pr = min(pix + int2(1, 0), maxPix);
    const int2 pu = min(pix + int2(0, 1), maxPix);
    const float zr = g_probeDepth.Load(int3(pr, 0)).x;
    const float zu = g_probeDepth.Load(int3(pu, 0)).x;
    if (zr >= 1.0f || zu >= 1.0f) return false;
    const float3 wr = gbReconstructWorldPos(uint2(pr), zr, size, g_invViewProj);
    const float3 wu = gbReconstructWorldPos(uint2(pu), zu, size, g_invViewProj);
    n = normalize(cross(wr - wp, wu - wp));
    if (dot(n, wp - g_eyeCell.xyz) < 0.0f) n = -n;   // 朝向相机（探针同口径）
    return true;
}

// R2 序列（低差异蓝噪声替身；帧号驱动 → 多帧覆盖 2×2 子像素，确定性）
float2 wcR2(uint n) {
    return float2(frac(n * 0.7548776662f), frac(n * 0.5698402909f));
}

// [0,1) 均匀随机（M11.1 update rng：mantissa 技巧；Slang 无 C++ lambda，用 inout）
float wcRand(inout uint rs) {
    rs = wcPcg(rs);
    return asfloat((rs & 0x007fffffu) | 0x3f800000u) - 1.0f;
}

// ---- (1) 半分辨率填充：屏幕采样 → 量化 → 查询即插入 ----
[shader("compute")]
[numthreads(8, 8, 1)]
void csCacheFill(uint3 dtid : SV_DispatchThreadID)
{
    const float2 size = float2(g_params.x, g_params.y);
    const uint2 pix = dtid.xy * 2u + uint2(wcR2(g_params.w) * 2.0f);  // 2×2 块内抖动
    if (pix.x >= uint(size.x) || pix.y >= uint(size.y)) return;

    const float deviceZ = g_probeDepth.Load(int3(pix, 0)).x;
    if (deviceZ >= 1.0f) return;                       // 天空：不入缓存

    const float3 wp = gbReconstructWorldPos(pix, deviceZ, size, g_invViewProj);
    float3 n;
    wcNormalFromDepth(int2(pix), wp, size, n);

    uint key, checksum;
    wcMakeKey(wp, n, key, checksum);
    [loop]
    for (uint i = 0u; i < kMaxProbeSteps; ++i) {
        uint existing;
        InterlockedCompareExchange(g_checksumsW[key], 0u, checksum, existing);
        if (existing == 0u) {
            // 抢到空槽：写几何 + 续命（viz 不读 geo，占用瞬态窗口无消费方）
            g_geoPosW[key] = float4(wp, 1.0f);
            g_geoNrmW[key] = float4(n, 0.0f);
            g_lifeW[key]   = uint(g_params.z);
            return;
        }
        if (existing == checksum) {
            g_lifeW[key] = uint(g_params.z);           // 命中：续命
            return;
        }
        key = (key + 1u) & (kCacheCapacity - 1u);      // 碰撞：线性探测
    }
    // 3 步内既没命中也没空槽：丢弃本样本（Carrot none 同语义，容量保护）
}

// ---- (2) 全表衰减：life-1，到 0 清 checksum 立即重用（bevy 同语义）----
[shader("compute")]
[numthreads(64, 1, 1)]
void csCacheDecay(uint idx : SV_DispatchThreadID)
{
    if (idx >= kCacheCapacity) return;
    uint life = g_lifeW[idx];
    if (life > 0u) {
        life -= 1u;
        g_lifeW[idx] = life;
        if (life == 0u) g_checksumsW[idx] = 0u;
    }
}

// ---- (3) M11.1 cell 更新：DI shadow ray + cosine 邻居射线查缓存 ----
// 口径与 gi-tools simCacheUpdate 逐字同构（CPU 仿真先行，22 断言已绿）：
//   cell 存入射辐照度 C；E_direct = lightCol·I·NdotL·shadow；
//   E_ind = albedo × C(hit)（lambert f=albedo/π × cos ÷ pdf=cos/π → albedo×L，
//   邻居出射 ≈ albedo × 邻居入射 C，统一 albedo）；缓存 miss → 0（多帧收敛
//   自然填充）；射线 miss 天空 → albedo × skyGradient(d)（bevy 的 π×env 是其
//   cell 存出射口径所致；我们入射口径下 π 被 pdf 消掉）。
// 软目标：pcg(idx^frame) 归一 < p（bevy target/count 的无 active-list 近似，
// 第一版固定 p；rng = cell_index + frame_index 入 seed，bevy 同款）。
// 新样本写 g_newRadianceW（w=1 标记），blend 只消费标记 cell——bevy active-list
// 语义在无 active-list 全表扫下的等价形态。
[shader("compute")]
[numthreads(64, 1, 1)]
void csCacheUpdate(uint idx : SV_DispatchThreadID)
{
    if (idx >= kCacheCapacity) return;
    if (g_checksumsR[idx] == 0u) return;               // 空槽
    if (wcPcg(idx ^ uint(g_params.w)) * (1.0f / 4294967296.0f) >= g_updateCtl.x) return;

    const float3 cp = g_geoPosR[idx].xyz;
    const float3 cn = g_geoNrmR[idx].xyz;

    // ---- DI 段：对光源 shadow ray（direct.slang g_shadowParams 口径）----
    const float3 origin = cp + cn * g_updateCol.w;
    float3 eDir = float3(0.0f, 0.0f, 0.0f);
    const float ndl = dot(cn, g_updateLight.xyz);
    if (ndl > 0.0f) {
        RayDesc sray;
        sray.Origin    = origin;
        sray.Direction = g_updateLight.xyz;
        sray.TMin      = 0.0f;
        sray.TMax      = g_updateMat.w;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(g_tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, sray);
        sq.Proceed();
        if (sq.CommittedStatus() == COMMITTED_NOTHING)
            eDir = g_updateCol.rgb * g_updateLight.w * ndl;
    }

    // ---- GI 段：cosine hemisphere 邻居射线（Malley disk，csReferenceGI 同款）----
    uint rs = wcPcg(idx) ^ uint(g_params.w);
    const float u1 = wcRand(rs), u2 = wcRand(rs);
    const float r = sqrt(u1), phi = 6.2831853f * u2;
    const float3 t1 = normalize(abs(cn.y) < 0.9f
                        ? cross(float3(0, 1, 0), cn) : cross(float3(1, 0, 0), cn));
    const float3 t2 = cross(cn, t1);
    const float3 d = normalize(t1 * (r * cos(phi)) + t2 * (r * sin(phi))
                               + cn * sqrt(max(1.0f - u1, 0.0f)));

    float3 eInd = float3(0.0f, 0.0f, 0.0f);
    RayDesc ray;
    ray.Origin    = origin;                            // 已带法线偏移
    ray.Direction = d;
    ray.TMin      = 0.001f;
    ray.TMax      = 1.0e4f;                            // 截太小漏天光（bevy 注释）
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_NOTHING) {
        eInd = g_updateMat.rgb * skyGradient(d);       // miss → 天穹（入射口径）
    } else {
        const float3 hitP = origin + d * q.CommittedRayT();
        // 命中点法线在 104-tri 粗代理上不可得（M10.1 降级同因）：用命中点
        // 位置 + 入射方向近似 cell key 的法线分量——以 -d 反向为表面朝向的
        // 一阶估计（凸面偏置可接受，多帧多方向摊平）。
        const float3 hn = -d;
        float3 hq = hitP + hn * g_updateCol.w;         // 沿法线微抬再量化（CPU 同构）
        uint key, checksum;
        wcMakeKey(hq, hn, key, checksum);
        [loop]
        for (uint i = 0u; i < kMaxProbeSteps; ++i) {
            const uint existing = g_checksumsR[key];
            if (existing == 0u) break;                 // 探测链撞空槽 = 必不存在
            if (existing == checksum) {
                eInd = g_updateMat.rgb * g_radianceR[key].xyz;   // 缓存反馈本体
                break;
            }
            key = (key + 1u) & (kCacheCapacity - 1u);
        }                                              // 缓存 miss → 0
    }

    g_newRadianceW[idx] = float4(eDir + eInd, 1.0f);   // w=1 = 本轮已选中标记
}

// ---- (4) M11.1 blend：线性时域混合，样本数 cap 64（自适应混合归 M11.3）----
// ⚠️ 必须消费后清 newRadiance 的 w 标记（CPU 版 simCacheBlend 同款）——
// 否则下帧 update 后未选中 cell 的旧标记残留，blend 会重复混合旧样本。
[shader("compute")]
[numthreads(64, 1, 1)]
void csCacheBlend(uint idx : SV_DispatchThreadID)
{
    if (idx >= kCacheCapacity) return;
    const float4 newRad = g_newRadianceR[idx];
    if (newRad.w <= 0.0f) return;                      // 本轮未选中：旧值原样保留
    const float4 old = g_radianceR[idx];
    const float samples = min(old.w, 63.0f);
    const float amt = 1.0f / (samples + 1.0f);         // 1/min(a+1, 64)
    g_radianceW[idx] = float4(lerp(old.xyz, newRad.xyz, amt), samples + 1.0f);
    g_newRadianceW[idx] = float4(newRad.xyz, 0.0f);    // 清标记（下帧 update 重写）
}

// ---- (5) viz：每像素 query-only 查表 → cell 分布伪彩 ----
[shader("compute")]
[numthreads(8, 8, 1)]
void csCacheViz(uint3 dtid : SV_DispatchThreadID)
{
    const float2 size = float2(g_params.x, g_params.y);
    const uint2 pix = dtid.xy;
    if (pix.x >= uint(size.x) || pix.y >= uint(size.y)) return;

    float3 rgbLin = float3(0.006f, 0.009f, 0.018f);    // miss/天空暗底（线性）
    const float deviceZ = g_probeDepth.Load(int3(pix, 0)).x;
    if (deviceZ < 1.0f) {
        const float3 wp = gbReconstructWorldPos(pix, deviceZ, size, g_invViewProj);
        float3 n;
        wcNormalFromDepth(int2(pix), wp, size, n);

        uint key, checksum;
        wcMakeKey(wp, n, key, checksum);
        [loop]
        for (uint i = 0u; i < kMaxProbeSteps; ++i) {
            const uint existing = g_checksumsR[key];
            if (existing == 0u) break;                 // 探测链撞空槽 = 必不存在
            if (existing == checksum) {
                if (g_updateCtl.y < 0.5f) {
                    // life 模式（M11.0 基线口径）：hash 伪彩 × 寿命亮度
                    const uint h = wcPcg(existing);
                    const float hue = float(h & 0xFFu) / 255.0f * 6.0f;
                    const float seg = floor(hue);
                    const float t = hue - seg;
                    if (seg < 1.0f)      rgbLin = float3(1, t, 0);
                    else if (seg < 2.0f) rgbLin = float3(1 - t, 1, 0);
                    else if (seg < 3.0f) rgbLin = float3(0, 1, t);
                    else if (seg < 4.0f) rgbLin = float3(0, 1 - t, 1);
                    else if (seg < 5.0f) rgbLin = float3(t, 0, 1);
                    else                 rgbLin = float3(1, 0, 1 - t);
                    const float life = float(g_lifeR[key]);
                    rgbLin *= 0.35f + 0.65f * life / max(g_params.z, 1.0f);
                } else {
                    // radiance 模式（M11.1）：出射辐照度 albedo·C/π 色调映射
                    // （直光面亮、阴影区只剩间接光变暗——cell 收敛肉眼可辨）
                    const float3 e = g_updateMat.rgb * g_radianceR[key].xyz
                                     * (1.0f / 3.14159265f);
                    rgbLin = e / (1.0f + e);
                }
                break;
            }
            key = (key + 1u) & (kCacheCapacity - 1u);
        }
    }
    // matResolve 产出是 gamma 编码的（M10.2 口径）：全替换像素同样回到编码域
    g_hdrOut[pix] = float4(pow(rgbLin, 1.0f / 2.2f), 1.0f);
}
