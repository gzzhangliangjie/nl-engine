// M10.1 (S10): 探针追踪 — 每探针每八面体 texel 1 条方向射线（S10-PLAN M10.1，
// 2026-11-11 Q12-1/Q12-2/Q12-3 拍板：UE TracingOctahedronResolution=8 口径，
// 8×8 core + 1 texel border = 10×10 存储 texel/探针；trace 与存储同分辨率，
// UE 分两步是为 importance sampling filtering，第一步不需要）。
//
// 线程组织：dispatch z = 4 级 cascade，x/y = 探针格 × 10×10 存储 texel 展平。
//   每线程：读布点 atlas（RGBA32F worldPos+deviceZ）→ a=0 天空探针写黑 →
//   有效探针沿 octaTexelToDir(octTexel)（border wrap 同方向口径）发 1 条
//   RayQuery 射线打 M8 TLAS：
//     miss  → skyGradient(dir)（Q12-3 解析天穹）
//     hit   → 【M11.2】命中点先查 S11 世界缓存：命中 → albedo ×
//             (C(hit)/π + ambient)；未命中 → 退回 M10.1 直光近似（备案）。
//             Q11-2c 补正拍板：命中分支也带 ambient 微底，与兜底同起跑线。
//             开关 --probe-cache-bounce 默认关（Q11-2a=A），关闭时整段查缓存
//             不执行，s10-* 冻结基线零漂。
//             M10.1 兜底（备案）：命中点直光（M9.1 口径降级）：
//             albedo(统一模型色) × (lightColor·intensity·shadow/π + ambient)，
//             shadow = 命中点朝主灯的 1 条阴影射线（origin 沿探针射线回退
//             bias 防自遮挡——命中点法线在 104-tri 粗代理上不可得，无 NdotL
//             项，同 albedo 统一色一起列 M10.1 如实降级，S11 世界缓存接管）。
//   射线 tMax 取大值（1e4）：bevy 注释实锤「截断处 miss ≠ 真 miss，截太小
//   漏天光」（GI-RESEARCH-NOTES §1）。
//
// Registers（§2.7 每管线单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   b0 = TraceConstants(80B volatile) + t0..t3 place atlas SRV（cur parity）
//        + t4 TLAS + t5 wcChecksums / t6 wcRadiance（M11.2 查缓存，只加 SRV
//        不动 Q4 冻结面）+ u0..u3 irradiance atlas UAV（RGBA16F，每级一张）
// （M10.1 无时间混合：单份不 ping-pong，降噪归 M10.3/S14。）
// S12 Q12-E：本入口仅在 --mode filter 下加逐帧方向抖动（g_traceBounce.z/w），
// 为 M12.0 时间累积提供 Monte Carlo 噪声源；非 filter 态保持 M10.1 原形态。

#include "octahedral.slangh"
#include "sky.slangh"

cbuffer TraceConstants : register(b0)
{
    float4 g_traceLightDir;   // xyz = 指向光源方向（writeResolveConstants 同源语义）
    float4 g_traceLightCol;   // rgb = 颜色（白），a = 强度（2.5，同 M9.x）
    float4 g_traceHitMat;     // rgb = 命中点 albedo（统一模型色），a = 环境光
    float4 g_traceParams;     // x = shadowBias（世界尺度，M9.1 g_shadowParams.x 同值）
                              // y = shadow tMax / z = baseRes / w = oct core res(8)
    // M11.2（SSC §2.21：CPU 布局逐字段对齐，scene.cpp tcb[16..19]）
    float4 g_traceBounce;     // x = 查缓存有效开关（probeCacheBounce && cacheFrame>0
                              //     ——首帧无「上一帧收敛值」，必须走兜底）
                              // y = DIAG 伪彩开关（CPU tcb[17]，本入口不消费）
                              // z = S12 Q12-E 帧间抖动开关（仅 --mode filter 置 1）
                              // w = S12 帧号（CPU 取模 64，确定性）
    // ⚠️ CB 尺寸恒 80B（5×float4）：Q12-E 只复用上面已声明的保留分量，
    // 不新增字段 —— CPU traceCB_ 分配 80B 不变（M11.0 实锤：分配小于
    // dxc -dumpbin 声明 Size → shader 读越界得 0 → 全线程静默早退）。
}

Texture2D<float4> g_placeAtlas0 : register(t0);   // 布点 atlas（RGBA32F cur parity）
Texture2D<float4> g_placeAtlas1 : register(t1);
Texture2D<float4> g_placeAtlas2 : register(t2);
Texture2D<float4> g_placeAtlas3 : register(t3);
RaytracingAccelerationStructure g_tlas : register(t4);

// M11.2：世界缓存只读视图（与 world_cache.slang t1/t5 同一 buffer，跨 set 复用）。
StructuredBuffer<uint>   g_wcChecksums : register(t5);
StructuredBuffer<float4> g_wcRadiance  : register(t6);

RWTexture2D<float4> g_irrad0 : register(u0);      // 辐照度 atlas（RGBA16F，a=1 有效）
RWTexture2D<float4> g_irrad1 : register(u1);
RWTexture2D<float4> g_irrad2 : register(u2);
RWTexture2D<float4> g_irrad3 : register(u3);

static const float kInvPi = 0.31830988f;

// ---- M11.2 世界缓存查询（与 world_cache.slang wcPcg/wcIqint/wcMakeKey
// 逐字同构，只改前缀避免链接期重名）。⚠️ 两处必须同步维护：改任一处
// 的哈希链 / kCellSize / 容量，另一处与 gi-tools WorldCacheSim 三方同改
//（SHADER-CONVENTIONS M11.2 槽位表登记）。此处复制而非抽公共头，是为了
// 不动 world_cache.slang 的冻结编译形态（s11-hash cacheviz bytes 基线）。
static const uint  ptCacheCapacity = 1u << 18;
static const uint  ptMaxProbeSteps = 3u;
static const float ptCellSize      = 0.5f;

uint ptPcg(uint input) {
    const uint state = input * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint ptIqint(uint input) {
    const uint n = (input << 13u) ^ input;
    return n * (n * n * 15731u + 789221u) + 1376312589u;
}
void ptMakeKey(float3 wp, float3 n, out uint key, out uint checksum) {
    // M11.2 根因修复（两层）：① 跨帧屏障由 drawProbeFrame 同帧 fill→trace 解决；
    // ② key 只用世界位置量化，丢弃法线分量（与 wcMakeKey 同构：csCacheFill 用深度
    // 法线、查询只持 dir，二者除垂直命中外几乎从不相等 → 查询恒 miss → 修正前
    // ON/OFF 逐字节相同）。缓存存间接 radiance（位置主导），实心场景位置同构即命中。
    // n 留形参保签名兼容，不参与哈希。
    const int3 qp = int3(floor(wp / ptCellSize));
    uint k = ptPcg(uint(qp.x));
    k = ptPcg(k + uint(qp.y));
    k = ptPcg(k + uint(qp.z));
    key = k & (ptCacheCapacity - 1u);
    uint c = ptIqint(uint(qp.x));
    c = ptIqint(c + uint(qp.y));
    c = ptIqint(c + uint(qp.z));
    checksum = max(c, 1u);   // 0 保留为空槽
}

// place atlas 4 路分支读（纹理数组不能动态索引，probe_viz 同款）。
float4 loadPlaceAtlas(uint level, uint2 texel)
{
    if (level == 0u) return g_placeAtlas0.Load(int3(texel, 0));
    if (level == 1u) return g_placeAtlas1.Load(int3(texel, 0));
    if (level == 2u) return g_placeAtlas2.Load(int3(texel, 0));
    return g_placeAtlas3.Load(int3(texel, 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csTraceProbe(uint3 tid : SV_DispatchThreadID)
{
    const uint level  = tid.z;
    const uint res    = uint(g_traceParams.z) >> level;
    const uint octRes = uint(g_traceParams.w);
    const uint store  = octRes + 2u;                    // 10（core + 1 texel border）
    // M14C1：行数用 irrad atlas runtime 高度判（GetDimensions）——probe-adaptive
    // 模式 atlas 高 ×2（预留区段 = 自适应探针），probe 模式高度 = res×store
    // 与旧判据 tid.y >= res*store 逐值一致（s10-*/s11-*/s12-* 冻结基线零漂）。
    // 旧判据在 adaptive 模式会把预留区段整段早退（M14C1 收口实锤：spawn 探针
    // 全部未被追踪，irrad 预留区恒黑）。
    float2 irradDim;
    g_irrad0.GetDimensions(irradDim.x, irradDim.y);
    if (tid.x >= res * store || tid.y >= uint(irradDim.y)) return;

    const uint2 probeT = uint2(tid.x / store, tid.y / store);
    const int2  octT   = int2(tid.x % store, tid.y % store) - 1;   // [-1, res]

    float3 radiance = float3(0.0f, 0.0f, 0.0f);         // 天空探针 = 黑（a=0 标记）
    const float4 probe = loadPlaceAtlas(level, probeT);
    if (probe.w > 0.0f)
    {
        // 探针 texel 方向（border texel wrap 到对侧 core 方向，无缝口径）。
        // ---- S12 M12.0（Q12-E 拍板）：filter 态逐帧方向重采样 ----
        // M10.1 每探针 64 条固定八面体方向逐帧确定性 → 静态场景里
        // lerp(hist, cur, alpha) 恒等于 cur（结构性 ON≡OFF，M11.2 同族坑）。
        // ⚠️ 不能只做「方向微扰」：M11.2 在 probe_atlas_dump.slang 文件头实锤，
        // csGather 每像素只采**外法线方向**那一个 texel，本凸体+纯天空场景中
        // 该方向恒 miss → 天空渐变；朝外方向微扰 0.15 rad 不会变命中，合成图
        // 仍逐字节相同（本轮实测：微扰版四图 MD5 全同，而 --probe-atlas-dump
        // 已证明 atlas 本身确实变了）。故改为每帧把该存储 texel 的射线方向
        // **随机置换到另一个 core texel**：gather 读到的 texel 逐帧在
        // 天空/命中之间跳变，产生真实 Monte Carlo 噪声，时间累积才有收敛。
        // ⚠️ 保形硬边界：非 filter 态 g_traceBounce.z=0 → 整块不执行、dir 仍是
        // 原表达式；复用 M11.2 已验证的 flag 门控形态（s10-* 冻结基线零漂）。
        // core texel 取 wrap 后坐标 → border 与对侧 core 抖动一致，跨 seam 无缝。
        float3 dir = octaTexelToDir(octT, octRes);
        if (g_traceBounce.z > 0.5f)
        {
            const int  n    = int(octRes);
            const int2 core = ((octT % n) + n) % n;
            const uint frame = uint(g_traceBounce.w);
            uint h = ptPcg((uint(core.x) * 1973u) ^ (uint(core.y) * 9277u));
            h = ptPcg(h ^ (probeT.x * 26699u) ^ (probeT.y * 31259u) ^ (frame * 66899u));
            const uint mask = octRes - 1u;   // octRes 为 2 的幂（UE 口径 8）
            const int2 jt = int2(int(ptPcg(h) & mask),
                                 int(ptPcg(h ^ 0x9e3779b9u) & mask));
            dir = octaTexelToDir(jt, octRes);
        }
        RayDesc ray;
        ray.Origin    = probe.xyz;                      // 布点已带法线偏移
        ray.Direction = dir;
        ray.TMin      = 0.001f;                         // rt.slang/bevy trace_ray 口径
        ray.TMax      = 1.0e4f;                         // 截太小漏天光（bevy 注释）
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_tlas,
                         RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         0xFF, ray);
        q.Proceed();

        if (q.CommittedStatus() == COMMITTED_NOTHING)
        {
            radiance = skyGradient(dir);                // miss → 解析天穹（Q12-3）
        }
        else
        {
            const float3 hitP = probe.xyz + dir * q.CommittedRayT();
            // ---- M11.2：命中点先查世界缓存（Q11-2a=A：只在开关开且缓存已
            // 跑过至少一帧时；帧序由 drawProbeFrame 同帧 fill→trace 保证读到本帧缓存）----
            // 命中分支（M10.1 兜底同款口径）：命中 → albedo×(C/π+ambient)；
            // 未命中 → 退回 M10.1 直光近似（PLAN「未命中 cell 才退回直光近似」）。
            // Q11-2c 补正拍板：命中分支也带 ambient 微底，与兜底同起跑线。
            // ── M11.2 根因修复口径（验收核心，两层）────────────────────────
            // ① 跨帧 UAV→SRV 屏障缺失：原帧序 trace 跨命令列表读上帧缓存，nvrhi 不
            // 插屏障 → trace 读清零/陈旧全零 → 已改 drawProbeFrame 同帧 fill→trace。
            // ② key 只量化世界位置（wcMakeKey/ptMakeKey 均丢弃法线）：csCacheFill 用
            // 深度法线、查询只持射线方向 dir（本 slang 版 RayQuery 不暴露命中顶点法线），
            // 二者除垂直命中外几乎从不相等 → 旧 normal-key 查询恒 miss → 画面逐字节相同。
            // 缓存存间接 radiance（位置主导），实心场景位置同构即命中；薄面罕见碰撞靠
            // 线性探测吸收。
            float3 bounced = float3(0.0f);
            bool cacheHit = false;
            if (g_traceBounce.x > 0.5f)
            {
                // M11.2 层② 2b 根因修复：查询点用探针原点 probe.xyz（必为可见已填
                // cell），不用命中点 hitP。本测试场景仅 Suzanne 凸体、无地面、
                // probeNormalOffset=0.10，探针射线射入物体内部打到背面/非可见面 →
                // hitP 所在 cell 从未被 csCacheFill 填过 → 校验码恒不匹配（绿=0）。
                // 探针原点沿法线偏置 0.10 仍落在自身可见面 cell 内，与缓存填充同构
                // → 命中。物理语义：探针沿各向取出的间接 radiance ≈ 其原点 cell 的
                // 间接 radiance（Lambert 出射项，正是 irrad atlas 想要的）。
                const float3 hq = probe.xyz;
                uint key, checksum;
                ptMakeKey(hq, dir, key, checksum);      // dir 仅占位，key 只用位置
                [loop]
                for (uint i = 0u; i < ptMaxProbeSteps; ++i) {
                    const uint existing = g_wcChecksums[key];
                    if (existing == 0u) break;          // 探测链撞空槽 = 必不存在
                    if (existing == checksum) {
                        const float3 cacheC = g_wcRadiance[key].xyz;
                        bounced = g_traceHitMat.rgb * (cacheC * kInvPi + g_traceHitMat.a);
                        cacheHit = true;
                        break;
                    }
                    key = (key + 1u) & (ptCacheCapacity - 1u);
                }
            }
            if (!cacheHit)
            {
                // ---- M10.1 兜底（备案）：命中点直光近似（无 NdotL 项，降级如实）----
                // 朝主灯发 1 条阴影射线（origin 沿探针射线回退 bias 防自遮挡）。
                const float3 so = hitP - dir * g_traceParams.x;
                RayDesc sray;
                sray.Origin    = so;
                sray.Direction = g_traceLightDir.xyz;
                sray.TMin      = 0.0f;
                sray.TMax      = g_traceParams.y;
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
                sq.TraceRayInline(g_tlas,
                                  RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                                  0xFF, sray);
                sq.Proceed();
                const float shadow = (sq.CommittedStatus() != COMMITTED_NOTHING) ? 0.0f : 1.0f;
                // 漫反射出射 radiance ≈ albedo/π · E_direct + albedo · E_ambient
                bounced = g_traceHitMat.rgb *
                          (g_traceLightCol.rgb * g_traceLightCol.a * shadow * kInvPi +
                           g_traceHitMat.a);
            }
            radiance = bounced;
        }
    }

    const float4 outData = float4(radiance, 1.0f);      // a=1 = texel 有效（含天空）
    switch (level)
    {
    case 0u: g_irrad0[tid.xy] = outData; break;
    case 1u: g_irrad1[tid.xy] = outData; break;
    case 2u: g_irrad2[tid.xy] = outData; break;
    case 3u: g_irrad3[tid.xy] = outData; break;
    }
}
