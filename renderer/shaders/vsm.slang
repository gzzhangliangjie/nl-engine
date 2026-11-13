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
//   [M15.2 剔除]    b1 = VsmCullConstants(288B) + t4/t5/t6/t7 + u1/u2/u3 + t8
//                   （剔除 set 与 hzb set 分绑，槽位表见下方各资源注释）
//   [M15.1 clear quad] 无额外资源（同 vsmDepthSet_，管线 blend 关=覆写 1.0）
// 入口：vsMain/psMain（M15.0 深度）+ vsMainClear/psMainClear（M15.1 清页 quad）
//       + csMark（M15.1 页请求标页）+ csHzb/csCullCount/csCullScan/csCullFill
//       （M15.2 两阶段剔除）。CMake _ENTRIES_VSM 逐入口出产物。
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

// M15.2：剔除/HZB 常量（b1，288B，剔除专用 set；与深度 pass b0 / mark b6 同文件
// 不同槽——set 独立互不冲突）。levelMvp[4] 与深度 pass b0 / mark b6 同源同帧；
// cullParams = {clusterCount, dirtyCount, virtualSize, vertsPerCluster}；
// hzbParams = {srcMip(0xFFFFFFFF=页池直采), dstMip, dstDim, poolDim(4096)}。
// CB 尺寸以 dxc -dumpbin 反汇编为准（288B，static_assert 对齐）。
cbuffer VsmCullConstants : register(b1)
{
    float4x4 g_cullLevelMvp[4];
    float4   g_cullParams;
    float4   g_hzbParams;
}

// M15.2 剔除资源（剔除 set；槽位接在深度 pass t1..t3 / mark t0 之后文件级唯一）：
//   t4 = 簇包围球（center.xyz + radius.w，与 cull.slang t2 同源——VSM 独立 SSBO，
//        cull 模式的 frustumCull_ 路径可能未建）
//   t5 = dirty 页列表 uint4(level, px, py, pad)（CPU writeBuffer，本帧序 = slot 序）
//   t6 = 页池（csHzb mip0 直采 2×2 min）
//   t7 = HZB（剔除遮挡判定读；buffer 形态 Load，同页表口径）
//   u1 = HZB（csHzb 写 + 递推读同 UAV——段间不重叠，上一级在前一 dispatch 写完）
//   u2 = cullBufs：[0,1024) count / [1024,2048) base（exclusive scan）/ [2048,3072)
//        cursor（fill 原子）/ [3072,3072+65536) 簇列表 / 尾 1 word 溢出旗标
//   u3 = per-page DrawIndirectArguments（4 word = {vertexCount, instanceCount, 0, 0}；
//        startInstanceLocation 恒 0——D3D12 SV_InstanceID 与 Vulkan gl_InstanceIndex
//        对 first-instance 语义相反，必须避开，双后端一致性）
StructuredBuffer<float4> g_cullSpheres : register(t4);
StructuredBuffer<uint4>  g_dirtyList   : register(t5);
Texture2D<float>         g_poolTex     : register(t6);
StructuredBuffer<uint>   g_hzbSrv      : register(t7);
RWStructuredBuffer<uint> g_hzbUav      : register(u1);
RWStructuredBuffer<uint> g_cullBufs    : register(u2);
RWStructuredBuffer<uint> g_cullArgs    : register(u3);

// M15.2：vs 深度 pass 读 cullBufs（base 段 + 簇列表段）——与剔除 CS 的 u2 同一
// buffer（NVRHI 跨 set SRV/UAV 分绑，同帧 UAV→SRV 状态转换）。
StructuredBuffer<uint>   g_vsCullBufs  : register(t8);

// HZB 布局：2048² 起步 12 级 min 链（mip0 = 页池 2×2 min；页 128 池 texel =
// 64 hzb texel，页级 = mip6）。单 StructuredBuffer 分段，offset 表 CPU/shader
// 双持（总 6553600 word ≈ 25MB）。
static const uint kHzbOffsets[12] = {
    0u,        4194304u,  5242880u,  5898240u,
    6225920u,  6389760u,  6471680u,  6512640u,
    6533120u,  6543360u,  6548480u,  6551040u
};
static const uint kHzbMip0Size = 2048u;
static const uint kCullListCap = 262144u;
static const uint kCullListBase = 3072u;

// ---- M15.2：簇 × dirty 页 判定（count/fill 共用，确定性同输入同结果）----
// 正交投影判交：簇球 → 光源 MVP（正交，w 恒 1）→ uv01 AABB；页矩形 =
// [px/16,(px+1)/16] × [1-(py+1)/16, 1-py/16]（uv01 空间，y 顶起口径与采样侧
// vtexel=(1-uv01.y)*virtualSize 逐字对称）。z 域 [0,1] 判交涵盖光源视锥深度。
// 遮挡：簇足迹自适应级采 HZB min（区域内最近已有深度），簇最近 z ≥ min → 被挡
// （bevy sample_hzb 同款近似；min-only 理论误剔洞登记 UE-DIFFS）。
bool vsmClusterPageVisible(uint level, uint px, uint py, float3 center, float radius)
{
    const float4x4 m = g_cullLevelMvp[level];
    const float4 clip = mul(m, float4(center, 1.0f));
    const float invW = rcp(clip.w);
    const float2 ndc = clip.xy * invW;
    const float  z   = clip.z * invW;
    // 正交投影线性：球投影半轴 = 行向量长度 × r / w（x/y 进 uv01，z 进深度域）。
    const float rx = length(m[0].xyz) * radius * invW;
    const float ry = length(m[1].xyz) * radius * invW;
    const float rz = length(m[2].xyz) * radius;
    const float2 u0 = (ndc - float2(rx, ry)) * 0.5f + 0.5f;
    const float2 u1 = (ndc + float2(rx, ry)) * 0.5f + 0.5f;
    // 足迹外扩 1 hzb texel：fp 舍入下顶点恰落页边界的簇不误剔（外扩只多画
    // 不漏画，MIN 混合无损性保持）。
    const float2 eps = 1.0f / float(kHzbMip0Size);

    if (z + rz < 0.0f || z - rz > 1.0f) return false;   // 光源 z 域外

    const float sx = 1.0f / 16.0f;
    const float2 pmin = float2(float(px), float(15u - py)) * sx;
    const float2 pmax = pmin + sx;
    if (u1.x + eps.x <= pmin.x || u0.x - eps.x >= pmax.x ||
        u1.y + eps.y <= pmin.y || u0.y - eps.y >= pmax.y) return false; // 页矩形外

    // ---- HZB 遮挡（第二阶段；页池上帧内容 = 静态场景精确深度）----
    const float2 f0 = max(float2(u0.x, 1.0f - u1.y), 0.0f) * float(kHzbMip0Size);
    const float2 f1 = min(float2(u1.x, 1.0f - u0.y), 1.0f) * float(kHzbMip0Size);
    const float  fw = max(f1.x - f0.x, 1.0f);
    const float  fh = max(f1.y - f0.y, 1.0f);
    uint k = 0u;
    const uint mwh = uint(max(fw, fh));
    while ((1u << k) < mwh && k < 11u) ++k;             // 足迹自适应级
    const uint dim  = kHzbMip0Size >> k;
    const uint2 tp  = min(uint2(f0) >> k, dim - 1u);
    float hzbMin = 1.0f;
    [unroll]
    for (uint dy = 0u; dy < 2u; ++dy)
        [unroll]
        for (uint dx = 0u; dx < 2u; ++dx) {
            const uint2 tc = min(tp + uint2(dx, dy), dim - 1u);
            hzbMin = min(hzbMin, asfloat(g_hzbSrv[kHzbOffsets[k] + tc.y * dim + tc.x]));
        }
    if (z - rz >= hzbMin && g_hzbParams.w > 0.0f) return false;   // HZB 遮挡判定（w>0 启用）
    return true;
}

// ---- M15.2：csHzb —— 页池 → 12 级 min 链（每级一次 dispatch，CB 覆写级参数）----
// mip0 从页池 2×2 min（页池 4096² SRV Load）；mip1+ 递推读同 buffer 上一级段
//（UAV 读，段间不重叠；上一级写完于前一 dispatch，GPU 全局有序）。
// 存 f32 bits（uint）：min = 最近深度（页池 0=near 口径）。
[numthreads(8, 8, 1)]
void csHzb(uint3 tid : SV_DispatchThreadID)
{
    const uint dstMip = uint(g_hzbParams.y);
    const uint dstDim = uint(g_hzbParams.z);
    if (tid.x >= dstDim || tid.y >= dstDim) return;

    float d;
    if (g_hzbParams.x < 0.0f) {                          // srcMip<0 = 页池直采
        const int3 b = int3(2 * int(tid.x), 2 * int(tid.y), 0);
        const float d00 = g_poolTex.Load(b);
        const float d10 = g_poolTex.Load(b + int3(1, 0, 0));
        const float d01 = g_poolTex.Load(b + int3(0, 1, 0));
        const float d11 = g_poolTex.Load(b + int3(1, 1, 0));
        d = min(min(d00, d10), min(d01, d11));
    } else {
        const uint srcDim = dstDim * 2u;
        const uint srcBase = kHzbOffsets[dstMip - 1u];
        const uint2 s = tid.xy * 2u;
        const uint r0 = srcBase + s.y * srcDim;
        const uint r1 = r0 + srcDim;
        d = min(min(asfloat(g_hzbUav[r0 + s.x]),     asfloat(g_hzbUav[r0 + s.x + 1u])),
                min(asfloat(g_hzbUav[r1 + s.x]),     asfloat(g_hzbUav[r1 + s.x + 1u])));
    }
    g_hzbUav[kHzbOffsets[dstMip] + tid.y * dstDim + tid.x] = asuint(d);
}

// ---- M15.2：csCullCount —— 簇 × dirty 页 判定 + 每页计数（2D 线程：x=簇，y=slot）----
// CPU 先把 cullBufs 头 3072 word 清零（count/base/cursor 段）。
[numthreads(64, 1, 1)]
void csCullCount(uint3 tid : SV_DispatchThreadID)
{
    const uint clusterCount = uint(g_cullParams.x);
    const uint dirtyCount   = uint(g_cullParams.y);
    if (tid.x >= clusterCount || tid.y >= dirtyCount) return;

    const uint4 pg = g_dirtyList[tid.y];
    const float4 sph = g_cullSpheres[tid.x];
    if (vsmClusterPageVisible(pg.x, pg.y, pg.z, sph.xyz, sph.w))
        InterlockedAdd(g_cullBufs[tid.y], 1u);
}

// ---- M15.2：csCullScan —— 单 group exclusive scan → base 段 + per-page args ----
// args[slot] = {vertsPerCluster, count, 0, 0}（DrawIndirectArguments 布局，
// startVertex/startInstance 恒 0）；list cap 由 fill 阶段防溢出。
groupshared uint sg_counts[1024];
[numthreads(1024, 1, 1)]
void csCullScan(uint gid : SV_GroupThreadID)
{
    const uint n = uint(g_cullParams.y);
    const uint c = (gid < n) ? g_cullBufs[gid] : 0u;
    sg_counts[gid] = c;
    GroupMemoryBarrierWithGroupSync();

    uint incl = c;                                       // Hillis-Steele inclusive
    [unroll]
    for (uint stride = 1u; stride < 1024u; stride <<= 1u) {
        const uint v = (gid >= stride) ? sg_counts[gid - stride] : 0u;
        GroupMemoryBarrierWithGroupSync();
        incl += v;
        sg_counts[gid] = incl;
        GroupMemoryBarrierWithGroupSync();
    }
    if (gid >= n) return;
    const uint excl = incl - c;
    g_cullBufs[1024u + gid] = excl;
    // uint() 数值转换（cullParams.w 是 float vertsPerCluster；asuint 是位型
    // 重解释——192.0f 会变成 11.4 亿顶点数，DrawInstanced 直接 TDR，实拍）。
    const uint vpc = uint(g_cullParams.w);
    g_cullArgs[gid * 4u + 0u] = vpc;
    // instanceCount clamp：溢出（base+count > cap）时截断，vsMain 读列表永不
    // 越 buffer 尾（截断丢簇 = 溢出语义，旗标在 fill 记账）。
    g_cullArgs[gid * 4u + 1u] = min(c, kCullListCap - excl);
    g_cullArgs[gid * 4u + 2u] = 0u;
    g_cullArgs[gid * 4u + 3u] = 0u;
}

// ---- M15.2：csCullFill —— 判定重跑（与 count 同输入同判定）→ 段式簇列表 ----
// 段内顺序不定（原子 cursor），列表只要求无序集合（MIN 混合 instance 顺序无关，
// cull.slang 同款论证）。
[numthreads(64, 1, 1)]
void csCullFill(uint3 tid : SV_DispatchThreadID)
{
    const uint clusterCount = uint(g_cullParams.x);
    const uint dirtyCount   = uint(g_cullParams.y);
    if (tid.x >= clusterCount || tid.y >= dirtyCount) return;

    const uint4 pg = g_dirtyList[tid.y];
    const float4 sph = g_cullSpheres[tid.x];
    if (!vsmClusterPageVisible(pg.x, pg.y, pg.z, sph.xyz, sph.w)) return;

    uint idx;
    InterlockedAdd(g_cullBufs[2048u + tid.y], 1u, idx);
    const uint base = g_cullBufs[1024u + tid.y];
    if (base + idx < kCullListCap)
        g_cullBufs[kCullListBase + base + idx] = tid.x;
    else
        InterlockedOr(g_cullBufs[kCullListBase + kCullListCap], 1u);   // 溢出旗标
}

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
    // g_pageInfo 取（per-dirty-page 覆写，位型打包）。
    const uint packed    = asuint(g_pageInfo.x);
    const uint winX      = packed & 15u;
    const uint winY      = (packed >> 4u) & 15u;

    // M15.2 间接簇列表：instanceIndex（恒 0 起，startInstanceLocation 恒 0——
    // 双后端 SV_InstanceID/g_InstanceIndex 语义差异规避）→ 页可见簇段寻址。
    // slot = b0 z 段（本帧 dirty 页序，与剔除 CS 的 slot 同源）；base = GPU 端
    // exclusive scan 产出；簇号 = 列表段 [base + instanceIndex]。
    const uint slot      = asuint(g_pageInfo.z);
    const uint listBase  = g_vsCullBufs[1024u + slot];
    if (listBase + instanceIndex >= kCullListCap) return output;   // 溢出截断守卫
    const uint clusterId = g_vsCullBufs[3072u + listBase + instanceIndex];

    const uint vertsPerCluster = asuint(g_pageInfo.y);
    const uint vertexInCluster = vertexIndex;
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
