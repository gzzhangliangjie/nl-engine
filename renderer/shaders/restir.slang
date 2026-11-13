// M14B1 (S14-B): ReSTIR GI 最小链 -- 两 pass（initial+temporal → spatial+shade），
// reservoir 两条 RWStructuredBuffer a/b ping-pong（Q-B3 拍板 bevy 同构）。
//
// 语义落点（Q-B1 拍板 B1-A）：GI 域 per-pixel 射线候选（RTXDI ReSTIR-GI 口径），
// 候选 = 余弦半球方向 + Ray Query 打 M8 TLAS + 命中点着色（S11 世界缓存间接 /
// M10.1 直光兜底 / miss 解析天穹）；reservoir 重用样本 = 「弹射方向 + 命中点 +
// Li」。bevy restir.wesl = ReSTIR-DI 定性（M14B0 实读），只移植机制不移植语义。
//
// 数学口径（B1 简化族，B2 收口复核）：
//   目标函数 p̂ = luminance(f)，贡献函数 f = albedo·Li·cosθ_r/π（cosθ_r = 弹射
//   方向与【本像素】法线 cos——邻居样本必须换算到本像素方向 dir_r =
//   normalize(hitPos − wp) 再评 p̂，合成 GI = f(dir_r) × W）。
//   W = weightSum / (m · p̂_sel)（Algorithm 4 无偏形态）+ [0, 8] 1-sample clamp
//   （RTXDI 量级口径防萤火，B2 复核）。
//   初始候选：M=1（Q-B2 拍板），dir = normalize(n + 单位球随机)（cosine 分布），
//   pdf = cosθ/π → w = p̂/pdf。
//   时域：velocity 重投影（velocity.xy = curUV − prevUV 唯一消费契约）→ 几何拒绝
//   （octa-64 法线 dot < 0.5 / 平面距离 > 5%·sceneDepth）→ 整池合并（w = p̂_r，
//   pdf = 1 确定性提案，m += prev.m，confidence cap = min(prev.conf, cap)）。
//   空域：5 邻居 disk（bevy 口径 30px 半径）+ 同族几何拒绝 + Jacobian 野值闸
//   （bevy 公式 J = phi_q·d_q²/(phi_r·d_r²)，[0.125, 8.0] 外拒绝，作几何不可能
//   重用的 outlier 防线；漫反射同 albedo 下 w 换算比 ≈ 1 的推导见 M14B0 §1，
//   B2 上 defensive MIS + 全 Jacobian 权重）→ w_j = p̂_r · nb.m，m += nb.m。
//
// Registers（§2.7 每管线单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   b0 = RestirConstants(144B volatile) + t0 probeDepth_ / t1 velocity_ /
//        t2 TLAS / t3 wcChecksums / t4 wcRadiance / t5 hdrSceneColor_ SRV
//        + u0 reservoirA / u1 reservoirB / u2 hdrSceneColor_ UAV。
//   两管线共用同一 BindingSet（probe_gather 同款口径）。
//   帧角色固定：pass1 读 A(上帧 spatial 末态) 写 B；pass2 读 B 写 A。

#include "octahedral.slangh"
#include "sky.slangh"
#include "gbuffer_common.slangh"

struct Reservoir
{
    float3 hitPos;        // 选中样本命中点（GI 域重用 = 方向+命中点）
    uint  packedNormal;   // 命中点法线 octa-64 texel 打包（几何拒绝用）
    float3 radiance;      // Li = 命中点出射 radiance（缓存/直光/天穹）
    float weightSum;      // Σ w_i（p̂/pdf 口径）
    float3 dir;           // 命中弹射方向（邻居侧；评 p̂ 用 dir_r 换算）
    float m;              // 样本池计数（cap 30 防爆炸）
    float confidence;     // 置信度（B2 defensive MIS 输入；B1 仅 cap 用）
    float unbiasedW;      // 最终无偏贡献权重 W = weightSum/(m·p̂_sel)
    float2 pad;           // 16 float = 64B 对齐
};

cbuffer RestirConstants : register(b0)
{
    float4x4 g_rInvViewProj;   // [0..15] 本帧 invViewProj（gatherCB_ 同源）
    float4   g_rLightDir;      // xyz = 指向光源方向
    float4   g_rLightCol;      // rgb = 颜色，a = 强度（traceCB_ 同源 2.5×ls）
    float4   g_rHitMat;        // rgb = 像素侧 albedo（统一暖灰），a = ambient
    float4   g_rParams;        // x = shadowBias, y = shadowTMax, z = viewW, w = viewH
    float4   g_rExtra;         // x = cacheOn, y = spatialRadius(px), z = confCap, w = frame
    float4   g_rEye;           // xyz = eye（法线朝向翻转），
                               // w = misLambda cap（B2 defensive MIS，scene 写 0.5）
}

Texture2D<float4> g_rDepth : register(t0);        // probeDepth_（velocity prepass 深度）
Texture2D<float4> g_rVel   : register(t1);        // velocity_（RG16F：curUV-prevUV）
RaytracingAccelerationStructure g_rTlas : register(t2);

// M11.2 世界缓存只读视图（probe_trace t5/t6 同 buffer 跨 set 复用）。
StructuredBuffer<uint>   g_rWcChecksums : register(t3);
StructuredBuffer<float4> g_rWcRadiance  : register(t4);

Texture2D<float4> g_rHdr : register(t5);          // hdrSceneColor_（SRV 读，RMW 合成）

RWStructuredBuffer<Reservoir> g_rResA : register(u0);   // 上帧末态（pass1 读 / pass2 写）
RWStructuredBuffer<Reservoir> g_rResB : register(u1);   // 本帧 temporal 末态
RWTexture2D<float4> g_rHdrUAV : register(u2);           // hdrSceneColor_（RMW 写）

static const float kInvPi = 0.31830988f;
static const uint  kPackRes = 64u;                     // octa 打包分辨率（-1..64 → 7bit+1）

// ---- RNG（probe_trace ptPcg 同源；seed 混入像素+帧，确定性截图口径）----
uint rsPcg(uint input) {
    const uint state = input * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint rsIqint(uint input) {
    const uint n = (input << 13u) ^ input;
    return n * (n * n * 15731u + 789221u) + 1376312589u;
}
float rsRand(inout uint seed) {
    seed = rsPcg(seed);
    return float(seed) * (1.0f / 4294967296.0f);
}
float3 rsRandSphere(inout uint seed) {
    const float z   = 1.0f - 2.0f * rsRand(seed);
    const float r   = sqrt(max(1.0f - z * z, 0.0f));
    const float phi = 6.2831853f * rsRand(seed);
    return float3(r * cos(phi), r * sin(phi), z);
}

// ---- 法线 octa 打包/解包（octaDirToTexel / octaTexelToDir round trip）----
uint rsPackNormal(float3 n) {
    const int2 t = octaDirToTexel(normalize(n), kPackRes);
    const uint px = uint(t.x + 1);    // [-1, 64] → [0, 65]
    const uint py = uint(t.y + 1);
    return (px << 8u) | py;
}
float3 rsUnpackNormal(uint packed) {
    const int2 t = int2(int((packed >> 8u) & 0xFFu) - 1,
                        int(packed & 0xFFu) - 1);
    return normalize(octaTexelToDir(t, kPackRes));
}

// ---- 世界缓存查询（probe_trace ptMakeKey 同构复制，前缀 rs；只读不改哈希链，
// 维护纪律见 probe_trace 文件头 SSC 登记）。
static const uint  rsCacheCapacity = 1u << 18;
static const uint  rsMaxProbeSteps = 3u;
static const float rsCellSize      = 0.5f;

bool rsCacheLookup(float3 wp, out float3 radiance)
{
    radiance = float3(0.0f, 0.0f, 0.0f);
    const int3 qp = int3(floor(wp / rsCellSize));
    uint key = rsPcg(uint(qp.x));
    key = rsPcg(key + uint(qp.y));
    key = rsPcg(key + uint(qp.z));
    key = key & (rsCacheCapacity - 1u);
    uint c = rsIqint(uint(qp.x));
    c = rsIqint(c + uint(qp.y));
    c = rsIqint(c + uint(qp.z));
    const uint checksum = max(c, 1u);
    [loop]
    for (uint i = 0u; i < rsMaxProbeSteps; ++i) {
        const uint existing = g_rWcChecksums[key];
        if (existing == 0u) return false;          // 探测链撞空槽 = 必不存在
        if (existing == checksum) {
            radiance = g_rWcRadiance[key].xyz;
            return true;
        }
        key = (key + 1u) & (rsCacheCapacity - 1u);
    }
    return false;
}

// ---- GBuffer：反投影 + 深度 3-tap 法线（probe_gather 同式改名版；纯 compute
// 无 ddx/ddy）。
float3 rsReconstruct(uint2 pix, float deviceZ, float2 size) {
    const float4 ndc = float4(
        (float(pix.x) + 0.5f) / size.x * 2.0f - 1.0f,
        1.0f - (float(pix.y) + 0.5f) / size.y * 2.0f,
        deviceZ, 1.0f);
    const float4 world = mul(g_rInvViewProj, ndc);
    return world.xyz / max(world.w, 1e-6f);
}
bool rsNormalFromDepth(int2 pix, float3 wp, float2 size, out float3 n) {
    n = float3(0.0f, 1.0f, 0.0f);
    const int2 maxPix = int2(size) - 1;
    const int2 pr = min(pix + int2(1, 0), maxPix);
    const int2 pu = min(pix + int2(0, 1), maxPix);
    const float zr = g_rDepth.Load(int3(pr, 0)).x;
    const float zu = g_rDepth.Load(int3(pu, 0)).x;
    if (zr >= 1.0f || zu >= 1.0f) return false;
    const float3 wr = rsReconstruct(uint2(pr), zr, size);
    const float3 wu = rsReconstruct(uint2(pu), zu, size);
    n = normalize(cross(wr - wp, wu - wp));
    if (dot(n, wp - g_rEye.xyz) > 0.0f) n = -n;
    return true;
}

// ---- 候选着色（probe_trace csTraceProbe 命中分支同构逐像素化）：
// miss → 解析天穹（outHit=false）；hit → 世界缓存命中 albedo×(C/π+ambient)，
// 未命中退直光兜底（朝主灯 1 条阴影射线）。outHitP = 真实命中点。
float3 rsShadeCandidate(float3 wp, float3 dir, bool cacheOn,
                        out float3 outHitP, out bool outMiss)
{
    outMiss = true;
    outHitP = wp;
    RayDesc ray;
    ray.Origin    = wp;              // 可见表面点（csCacheFill 同构 cell，缓存可命中）
    ray.Direction = dir;
    ray.TMin      = 0.001f;
    ray.TMax      = 1.0e4f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_rTlas,
                     RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                     0xFF, ray);
    q.Proceed();

    if (q.CommittedStatus() == COMMITTED_NOTHING) {
        return skyGradient(dir);
    }

    outMiss = false;
    const float3 hitP = wp + dir * q.CommittedRayT();
    outHitP = hitP;

    float3 bounced = float3(0.0f, 0.0f, 0.0f);
    bool cacheHit = false;
    if (cacheOn) {
        float3 cacheC;
        if (rsCacheLookup(wp, cacheC)) {           // 查询点 = 像素表面点（可见已填 cell）
            bounced = g_rHitMat.rgb * (cacheC * kInvPi + g_rHitMat.a);
            cacheHit = true;
        }
    }
    if (!cacheHit) {
        const float3 so = hitP - dir * g_rParams.x;
        RayDesc sray;
        sray.Origin    = so;
        sray.Direction = g_rLightDir.xyz;
        sray.TMin      = 0.0f;
        sray.TMax      = g_rParams.y;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(g_rTlas,
                          RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                          0xFF, sray);
        sq.Proceed();
        const float shadow = (sq.CommittedStatus() != COMMITTED_NOTHING) ? 0.0f : 1.0f;
        bounced = g_rHitMat.rgb *
                  (g_rLightCol.rgb * g_rLightCol.a * shadow * kInvPi + g_rHitMat.a);
    }
    return bounced;
}

// 贡献函数 f = albedo·Li·cosθ_r/π；target p̂ = luminance(f)（+ε 防全零）。
float rsTarget(float3 li, float3 dirFromPixel, float3 n) {
    const float cosT = max(dot(dirFromPixel, n), 0.0f);
    const float3 f = g_rHitMat.rgb * li * (cosT * kInvPi);
    return dot(f, float3(0.2126f, 0.7152f, 0.0722f)) + 1e-6f;
}

// ---- reservoir 合并（Algorithm 4 形态，显式 w / mIncr / conf）。
void rsReservoirMerge(inout Reservoir r, float3 hitPos, float3 nHit, float3 radiance,
                      float3 dir, float w, float mIncr, float conf, inout uint seed)
{
    r.weightSum += w;
    r.m += mIncr;
    r.confidence += conf;
    if (rsRand(seed) < w / max(r.weightSum, 1e-6f)) {
        r.hitPos = hitPos;
        r.packedNormal = rsPackNormal(nHit);
        r.radiance = radiance;
        r.dir = dir;
    }
}

Reservoir rsEmptyReservoir() {
    Reservoir r;
    r.hitPos = float3(0, 0, 0); r.packedNormal = 0u;
    r.radiance = float3(0, 0, 0); r.weightSum = 0.0f;
    r.dir = float3(0, 1, 0); r.m = 0.0f;
    r.confidence = 0.0f; r.unbiasedW = 0.0f; r.pad = float2(0, 0);
    return r;
}

// ---- pass1：初始候选 + 时域重用（写 reservoirB）----
[shader("compute")]
[numthreads(8, 8, 1)]
void csRestirTemporal(uint3 tid : SV_DispatchThreadID)
{
    const float viewW = g_rParams.z;
    const float viewH = g_rParams.w;
    const uint idx = tid.x + tid.y * uint(viewW);
    const float2 size = float2(viewW, viewH);

    Reservoir r = rsEmptyReservoir();

    const float deviceZ = g_rDepth.Load(int3(int2(tid.xy), 0)).x;
    if (deviceZ <= 0.0f || deviceZ >= 1.0f) {
        g_rResB[idx] = r;                        // 天空：空 reservoir（契约照写）
        return;
    }

    const float3 wp = rsReconstruct(tid.xy, deviceZ, size);
    float3 n;
    rsNormalFromDepth(int2(tid.xy), wp, size, n);

    uint seed = rsPcg(uint(tid.x) * 1973u ^ uint(tid.y) * 9277u
                            ^ uint(g_rExtra.w) * 66899u);

    // ---- 初始候选 M=1：cos 半球方向 = normalize(n + 单位球随机)。
    float3 sdir = normalize(n + rsRandSphere(seed));
    if (dot(sdir, sdir) < 0.5f) sdir = n;        // 数值防线（概率 ~0）

    float3 hitP; bool miss;
    const float3 li = rsShadeCandidate(wp, sdir, g_rExtra.x > 0.5f, hitP, miss);
    const float pdf0 = max(max(dot(sdir, n), 0.0f) * kInvPi, 1e-6f);
    const float target0 = rsTarget(li, sdir, n);
    rsReservoirMerge(r, hitP, n, li, sdir, target0 / pdf0, 1.0f, 1.0f, seed);

    // ---- 时域重用：velocity.xy = curUV − prevUV → prevPix = (uv − vel)·size。
    const float2 vel = g_rVel.Load(int3(int2(tid.xy), 0)).xy;
    const float2 prevUV = (float2(tid.xy) + 0.5f) / size - vel;
    const int2 prevPix = int2(floor(prevUV * size));
    if (prevPix.x >= 0 && prevPix.y >= 0
        && prevPix.x < int(viewW) && prevPix.y < int(viewH)) {
        const uint prevIdx = uint(prevPix.x) + uint(prevPix.y) * uint(viewW);
        const Reservoir prev = g_rResA[prevIdx];
        const float3 prevN = rsUnpackNormal(prev.packedNormal);
        const bool gOk = prev.m > 0.0f && prev.confidence > 0.0f && prev.packedNormal != 0u
            && dot(n, prevN) > 0.5f
            && abs(dot(prev.hitPos - wp, n)) < 0.05f * max(length(wp - g_rEye.xyz), 1e-3f);
        if (gOk) {
            // 整池合并：pdf=1 确定性提案（w = p̂_r），m += prev.m，
            // confidence cap（bevy 口径 min(conf, cap)）。
            const float pT = rsTarget(prev.radiance, normalize(prev.hitPos - wp), n);
            rsReservoirMerge(r, prev.hitPos, prevN, prev.radiance, prev.dir,
                             pT, prev.m, min(prev.confidence, g_rExtra.z), seed);
        }
    }

    // W = weightSum/(m·p̂_sel)；p̂_sel 恒用本像素方向 dir_r。
    const float pSel = rsTarget(r.radiance, normalize(r.hitPos - wp), n);
    r.unbiasedW = (r.m > 0.0f) ? r.weightSum / (r.m * max(pSel, 1e-6f)) : 0.0f;
    g_rResB[idx] = r;
}

// ---- pass2：空间重用 + shade + RMW 合成（写 reservoirA + hdrSceneColor_）----
[shader("compute")]
[numthreads(8, 8, 1)]
void csRestirSpatial(uint3 tid : SV_DispatchThreadID)
{
    const float viewW = g_rParams.z;
    const float viewH = g_rParams.w;
    const uint idx = tid.x + tid.y * uint(viewW);
    const float2 size = float2(viewW, viewH);
    const float radius = g_rExtra.y;

    Reservoir r = g_rResB[idx];
    if (r.m <= 0.0f || r.confidence <= 0.0f || r.packedNormal == 0u) {
        g_rResA[idx] = r;                        // 天空：空契约回写 A（下帧时域读）
        return;
    }

    const float deviceZ = g_rDepth.Load(int3(int2(tid.xy), 0)).x;
    if (deviceZ <= 0.0f || deviceZ >= 1.0f) { g_rResA[idx] = r; return; }
    const float3 wp = rsReconstruct(tid.xy, deviceZ, size);
    float3 n;
    rsNormalFromDepth(int2(tid.xy), wp, size, n);
    const float sceneDepth = length(wp - g_rEye.xyz);

    uint seed = rsPcg(uint(tid.x) * 1973u ^ uint(tid.y) * 9277u
                            ^ uint(g_rExtra.w) * 66899u ^ 0x9e3779b9u);

    // ---- 空域重用：5 邻居 disk 采样（bevy 口径半径 CB 可调）。
    for (uint k = 0u; k < 5u; ++k) {
        const float ang = rsRand(seed) * 6.2831853f;
        const float rad = sqrt(rsRand(seed)) * radius;
        const int2 nbPix = int2(tid.xy)
            + int2(int2(round(cos(ang) * rad), round(sin(ang) * rad)));
        if (nbPix.x < 0 || nbPix.y < 0
            || nbPix.x >= int(viewW) || nbPix.y >= int(viewH)) continue;
        const uint nbIdx = uint(nbPix.x) + uint(nbPix.y) * uint(viewW);
        const Reservoir nb = g_rResB[nbIdx];
        if (nb.m <= 0.0f || nb.confidence <= 0.0f || nb.packedNormal == 0u) continue;

        // 几何拒绝（bevy pixel_dissimilar 口径）：法线 dot + 平面距离。
        const float3 nbN = rsUnpackNormal(nb.packedNormal);
        if (dot(n, nbN) < 0.5f) continue;
        if (abs(dot(nb.hitPos - wp, n)) > 0.05f * max(sceneDepth, 1e-3f)) continue;

        // Jacobian 野值闸（bevy 公式 J = phi_q·d_q²/(phi_r·d_r²)）：
        // phi_q = 弹射方向与邻居表面法线 cos（nb.dir 来自邻居表面点），
        // d_q = 邻居表面点 → 命中点距离（邻居 wp 从深度反投影）；
        // phi_r / d_r = 本像素侧同义。漫反射同 albedo 下 w 换算比 ≈ 1，
        // J 只作几何不可能重用的 outlier 防线（[0.125, 8.0] 外拒绝）。
        const float nbZ = g_rDepth.Load(int3(nbPix, 0)).x;
        if (nbZ <= 0.0f || nbZ >= 1.0f) continue;
        const float3 nbWp = rsReconstruct(uint2(nbPix), nbZ, size);
        const float phiQ = max(dot(nb.dir, nbN), 1e-4f);
        const float dQ   = max(length(nb.hitPos - nbWp), 1e-3f);
        const float3 dirR = normalize(nb.hitPos - wp);
        const float phiR = max(dot(dirR, n), 1e-4f);
        const float dR   = max(length(nb.hitPos - wp), 1e-3f);
        const float J = (phiQ * dQ * dQ) / (phiR * dR * dR);
        if (J < 0.125f || J > 8.0f) continue;

        // 整池合并：w_j = p̂_r · nb.m（漫反射同 albedo 换算比 ≈ 1，M14B0 §1）。
        // B2 置信度加权：邻居贡献 × conf/cap（低置信邻居降权——RTXDI light
        // confidence 同思路；cap 归一到 [0,1]，confidence cap 照旧 min）。
        const float nbConf = min(nb.confidence, g_rExtra.z) / max(g_rExtra.z, 1e-3f);
        const float pJ = rsTarget(nb.radiance, dirR, n);
        rsReservoirMerge(r, nb.hitPos, nbN, nb.radiance, nb.dir,
                         pJ * nb.m * nbConf, nb.m, min(nb.confidence, g_rExtra.z), seed);
    }
    r.m = min(r.m, 30.0f);                       // 池计数 cap（防邻居爆炸）

    // W 收口（p̂_sel 恒用本像素方向 dir_r）+ 1-sample clamp（RTXDI 量级）。
    const float3 dirSel = normalize(r.hitPos - wp);
    const float pSel = rsTarget(r.radiance, dirSel, n);
    r.unbiasedW = clamp(r.weightSum / (r.m * max(pSel, 1e-6f)), 0.0f, 8.0f);

    // B2 defensive MIS（RTXDI misLambda 口径）：W = W_u·[(1-λ)+λ/m]。
    // λ = cap·(1-conf01)：池置信度越低越防守（用 1/m 均匀降方差换无偏性；
    // conf≥cap 后 λ=0 退回纯无偏 W）。cap = g_rEye.w（scene 恒写 0.5）。
    const float conf01 = saturate(r.confidence / max(g_rExtra.z, 1e-3f));
    const float lam = saturate(g_rEye.w) * (1.0f - conf01);
    r.unbiasedW *= (1.0f - lam) + lam / max(r.m, 1.0f);
    g_rResA[idx] = r;

    // ---- shade + compose（csGatherCompose 同式）：GI = f = albedo·Li·cosθ_r/π
    // （完整贡献函数 × W）；直光 encoded → 去伽马 → +GI → 回伽马（RMW 同 texel）。
    const float3 gi = g_rHitMat.rgb * r.radiance
                    * (max(dot(dirSel, n), 0.0f) * kInvPi) * r.unbiasedW;
    const float4 enc = g_rHdr.Load(int3(int2(tid.xy), 0));
    const float3 lin    = pow(saturate(enc.rgb), 2.2f);
    const float3 outLin = lin + gi;
    const float3 outEnc = pow(saturate(outLin), 1.0f / 2.2f);
    g_rHdrUAV[int2(tid.xy)] = float4(outEnc, enc.a);
}
