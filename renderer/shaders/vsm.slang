// M15.0 (S15): VSM 深度 pass —— 全簇 vertex pulling 渲虚拟阴影贴图。
//
// 几何口径与 visbuf.slang::vsVisRaster 同源（同 vertexPool/localIndices/clusterMeta、
// 同 NaN 裁剪守卫），两处差异：
//   1) 不读 visibleList——那是相机视锥剔除列表，会把相机外遮挡体漏掉（影不完整）。
//      M15.0 全簇 instance 直绘（instanceIndex 即簇号）；按光源视锥的逐页 UV 标记
//      剔除留 M15.2（UE BuildPerPageDrawCommands 对位）。
//   2) MVP = 光源正交 lightViewProj（CPU 侧 shadowOrtho/shadowLookAt 预算，E1b.4
//      同款：全场 AABB + 灯方向；M15.0 单光源单级，无 clipmap 层级）。
// PS 只输出 SV_Position.z（D3D 视口后 NDC z ∈ [0,1]）进 R32F 页池——接收者采样侧
// 用同一矩阵算 NDC.z 比较，双后端数值同源（0..1 深度范围 Vulkan/D3D 一致）。
//
// 资源寄存器（§2.7 每管线单一 BindingSet）：
//   b0 = VsmDepthConstants (80B)：g_lightMvp + 16B 对齐占位（CB 尺寸以 dxc -dumpbin 为准）
//   t1..t3 = vertexPool / localIndices / clusterMeta（与 model.slang/visbuf.slang 同源）
//   [M15.1 csMark]  b6 = VsmMarkConstants(336B) + t0 = visBuffer_（deviceZ 高位）
//                   + u0 = 请求位图（RWStructuredBuffer<uint> 32 word）
//   [M15.1 clear quad] 无额外资源（同 vsmDepthSet_，管线 blend 关=覆写 1.0）
// 入口：vsMain/psMain（M15.0 深度）+ vsMainClear/psMainClear（M15.1 清页 quad）
//       + csMark（M15.1 页请求标页）。CMake _ENTRIES_VSM 逐入口出产物。
// SPIR-V 平移同全家：b→256, t→0, s→128, u→384。

cbuffer VsmDepthConstants : register(b0)
{
    float4x4 g_lightMvp;     // lightViewProj（列主序，mul(M, v)）
    float4   g_pageInfo;     // 位型复用（asuint 解包，原 pad 从未被深度 shader 读取）：
                             //   x = packed：winX(4b) | winY<<4（CPU 侧只写这两段）
                             //   y = asuint(vertsPerCluster)（全局 vertexIndex → 簇号除数）
                             // z/w 未用。per-dirty-page draw 覆写（volatile 版本上限
                             // 2048），CB 总 80B 不变（dxc -dumpbin 纪律）。
}

// M15.1：标页常量（b6，336B，marking CS 专用 set；与 csDirect 的 b6 同槽不同 set）。
// invViewProj = inverseMat4(params.viewProj)（CPU，nl/math inverseMat4，probe 同款）；
// levelMvp[4] = 每级光源正交（与深度 pass b0 的 g_lightMvp 同源同帧）；
// depthSize = (w, h, 0, 0)（visBuffer_ 线性索引用）。
cbuffer VsmMarkConstants : register(b6)
{
    float4x4 g_markInvViewProj;
    float4x4 g_markLevelMvp[4];
    float4   g_markParams;      // x = visW, y = visH, z = virtualSize, w 未用
}

StructuredBuffer<float> g_vertexPool   : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);

// M15.1 标页输入/输出（独立 set，槽位与深度 pass 不冲突）：
//   t0 = visBuffer_（u64/px：高 32 = asuint(deviceZ)，低 32 = clusterId<<8|triId）
//   u0 = 请求位图 32 word：level L 占 word[L*8 + (pageIdx>>5)]，
//        pageIdx = py*16+px ∈ [0,256)，bit = pageIdx&31。
StructuredBuffer<uint64_t> g_markVis  : register(t0);
RWStructuredBuffer<uint>   g_markBits : register(u0);

struct VSVsmOutput
{
    float4 position : SV_Position;
};

VSVsmOutput vsMain(uint instanceIndex : SV_InstanceID,
                   uint vertexIndex   : SV_VertexID)
{
    VSVsmOutput output;
    // padding 三角 / 越界一律 NaN 裁剪坐标，光栅化阶段整三角丢弃（M4 同款）。
    output.position = asfloat(0x7fc00000u).xxxx;

    // M15.1 instance remap：每页一次 draw（instanceCount=1），页参数从 b0
    // g_pageInfo 取（per-dirty-page 覆写，位型打包）。簇号改由全局 vertexIndex /
    // vertsPerCluster 解码（b0 pad.y）。
    const uint packed    = asuint(g_pageInfo.x);
    const uint winX      = packed & 15u;
    const uint winY      = (packed >> 4u) & 15u;

    const uint vertsPerCluster = asuint(g_pageInfo.y);
    const uint clusterId  = vertexIndex / vertsPerCluster;
    const uint vertexInCluster = vertexIndex - clusterId * vertsPerCluster;
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint triCount   = g_clusterMeta[metaBase + 1u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];

    const uint triInCluster = vertexInCluster / 3u;
    if (triInCluster >= triCount) return output;

    const uint localIdx = g_localIndices[localBase + vertexInCluster];
    const uint gv = vertexBase + localIdx;
    const uint b  = gv * 8u;                 // 8 float / 顶点（pos/normal/uv）
    const float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);

    // 窗口 NDC（[-1,1] = 2048 texel 虚拟窗）→ 本页窗口子区 [ax, ax+sx] → 页内
    // NDC [-1,1]（viewport = 页矩形 128²，CPU 侧 per-draw 设置）。子区外几何
    // 映射到 |ndc_local| > 1 → clip space 规范盒裁剪（替代旧方案的 scissor+放
    // 大 viewport——后者在 VK 后端产生确定性页丢失/texel 丢失，页池读回实锤）。
    // 必须先透视除法再线性 remap 再乘回 w（透视除法对线性 remap 不可交换）。
    // y 口径：页索引 py = 虚拟 texel 空间向下（py=0 = NDC y=+1 图顶，csMark/
    // vsmSampleShadow4 的 vtexel=(1-uv01.y) 同款）——子区起 ay = 1 - sx*(winY+1)
    // 覆盖 NDC 顶部；写成 -1+sx*winY 会把页内容上下镜像（M15.0 漏屏幕翻转同款
    // 坑）。x 无翻转（px=0 = NDC 左）。
    // [-1,1] 因子：(ndc-ax)/sx 只把子区映到 [0,1]，页 NDC 是 [-1,1]——漏 ×2-1
    // 会把窗口子区压缩进页右下 1/4（页内 texel 与采样侧错 2 倍 + 随 winY 漂移，
    // VSM vs ray near 实拍 5.29% 页行条纹假阴影实锤）。逐 texel 推导：窗口
    // texel (winX*128+i, winY*128+j) ↔ 页内 texel (i,j)，×2-1 后两侧逐 texel 对齐。
    const float4 clip = mul(g_lightMvp, float4(pos, 1.0f));
    const float invW  = rcp(clip.w);
    const float ndcx  = clip.x * invW;
    const float ndcy  = clip.y * invW;
    const float sx = 2.0f / 16.0f;                 // 页的窗口 NDC 跨度（128/1024 texel）
    const float ax = -1.0f + sx * float(winX);     // 本页子区 NDC 起
    const float ay = 1.0f - sx * (float(winY) + 1.0f);
    const float rx = (ndcx - ax) / sx * 2.0f - 1.0f;   // 子区 → 页内 NDC [-1,1]
    const float ry = (ndcy - ay) / sx * 2.0f - 1.0f;
    output.position = float4(rx * clip.w, ry * clip.w, clip.z, clip.w);
    return output;
}

float4 psMain(float4 position : SV_Position) : SV_Target
{
    // R32F RT 取 .x：SV_Position.z = 视口后 NDC z（[0,1]），与接收者侧同口径。
    return float4(position.z, 0.0f, 0.0f, 0.0f);
}

// ---- M15.1：清页 quad（新分配页先覆写 1.0 再画几何）----
// MIN 混合无法把旧占用者的深度「重置」回 1.0（min(1, stale)=stale），故清页
// 用 blend 关的覆写管线：全屏三角形（SV_VertexID 生成，无顶点缓冲）铺满当前
// viewport（= 页矩形 128²，CPU 侧 per-draw 设置），PS 输出常量 1.0（最远）。
VSVsmOutput vsMainClear(uint vertexIndex : SV_VertexID)
{
    VSVsmOutput output;
    // 全屏大三角形：(-1,-1) (3,-1) (-1,3)——覆盖整个 viewport。
    const float2 ndc = float2((vertexIndex == 1u) ? 3.0f : -1.0f,
                              (vertexIndex == 2u) ? 3.0f : -1.0f);
    output.position = float4(ndc, 1.0f, 1.0f);
    return output;
}

float4 psMainClear(float4 position : SV_Position) : SV_Target
{
    return float4(1.0f, 0.0f, 0.0f, 0.0f);   // R32F 通道 0 = 深度 1.0（最远）
}

// ---- M15.1：csMark 页请求标页（UE VirtualShadowMapPageMarking.usf 对位）----
// 8×8 线程组逐像素：visBuffer_ 高位取 deviceZ（VIS_EMPTY=背景跳过）→
// gbReconstructWorldPos 同款数学重建世界坐标 → 逐级投影，命中页 InterlockedOr
// 进请求位图。CPU 分配器延迟两帧消费位图（cullReadback_ 同款环形）。
// 页坐标推导与 vsm_common.slangh::vsmSampleShadow4 逐字对称（含 y 翻转）。
[numthreads(8, 8, 1)]
void csMark(uint3 tid : SV_DispatchThreadID)
{
    const uint width  = uint(g_markParams.x);
    const uint height = uint(g_markParams.y);
    if (tid.x >= width || tid.y >= height) return;

    const uint64_t v = g_markVis[tid.y * width + tid.x];
    if (v == 0xFFFFFFFFFFFFFFFFu) return;    // VIS_EMPTY：背景像素不产生页请求
    const float deviceZ = asfloat(uint(v >> 32u));

    // 世界坐标重建（gbuffer_common.slangh::gbReconstructWorldPos 同款数学，
    // perspectiveZO：ndc z=0 near / z=1 far）。
    const float2 uv  = (float2(tid.xy) + 0.5f) / float2(g_markParams.x, g_markParams.y);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h   = mul(g_markInvViewProj, float4(ndc, deviceZ, 1.0f));
    const float3 wp  = h.xyz / h.w;

    const float virtualSize = g_markParams.z;
    // 每像素只请求最细命中级（UE 单级选择口径；全级标记 = 需求 4×256 页 > 池
    // 1024 会 LRU 抖动）。与 vsm_common.slangh::vsmSampleShadow4 的最细级优先
    // 采样口径一致：请求级 = 采样级。
    for (uint L = 0u; L < 4u; ++L) {
        const float4 clip = mul(g_markLevelMvp[L], float4(wp, 1.0f));
        const float2 uv01 = (clip.xy / clip.w) * 0.5f + 0.5f;
        if (uv01.x < 0.0f || uv01.x >= 1.0f || uv01.y < 0.0f || uv01.y >= 1.0f)
            continue;

        const float2 vtexel = float2(uv01.x, 1.0f - uv01.y) * virtualSize;
        const uint2 page = uint2(vtexel) >> 7u;           // PageSize=128
        const uint pageIdx = page.y * 16u + page.x;       // ∈ [0,256)
        InterlockedOr(g_markBits[L * 8u + (pageIdx >> 5u)], 1u << (pageIdx & 31u));
        break;
    }
}
