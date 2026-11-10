// M-PT0 particle overlay — single source of truth, dual target.
// 同一份 .slang 经 slangc 编出 SPIR-V（Vulkan）与 DXIL（DX12），约定与
// model.slang / debug_line.slang 完全一致（docs/SHADER-CONVENTIONS.md §1）。
//
// instanced billboard：无顶点缓冲、无 input layout——VS 用 SV_VertexID(0..5)
// 展开每实例两枚三角形的屏幕对角角点，SV_InstanceID 从 t0 StructuredBuffer
// 取粒子数据（UE「粒子注入主渲染」思路的等价落法：draw 挂在主场景之后、
// HUD 之前，alpha blend，深度测试开 / 深度写入关）。
//
// 资源（两后端同名寄存器，Vulkan 走 -fvk-b-shift 256 / -fvk-t-shift 0 / -fvk-s-shift 128）：
//   b0 = ParticleConstantsCB（volatile，每帧全量覆写，112 B）
//   t0 = StructuredBuffer<ParticleGpu>（每实例 32 B：posSize + colorAlpha）
//   t1 = 粒子贴图（nfx texture 字段；无贴图绑 1x1 白 dummy）
//   s0 = Linear clamp 采样器
//   g_params.y = 贴图开关（0 = 软圆盘路径，1 = 贴图采样）
//   g_params.z = Y/X 尺寸比（legacy/方形恒 1.0）
//
// 布局铁律：结构体与 scene.cpp 的 ParticleGpu / ParticleConstantsCB 逐字节
// 对应（32 B / 128 B），漂移会直接画花而不是报错——改任何一侧必须同步另一侧。

cbuffer ParticleConstantsCB : register(b0)
{
    // view-projection * model，列主序（与 model.slang g_mvp 同口径）。
    // 粒子世界坐标经它直接进裁剪空间——粒子只在 normal 系模式（model≈identity）
    // 下使用，世界空间即模型空间。
    float4x4 g_mvp;
    // 相机基向量（世界空间，列主序 view 矩阵的第 0/1 行）：
    // billboard 沿 right×up 展开，永远正对相机。
    float4 g_camRight;
    float4 g_camUp;
    // 预留（当前 x=整体尺寸倍率，其余未用）。
    float4 g_params;
};

struct ParticleGpu
{
    float4 posSize;    // xyz = 世界位置，w = 尺寸 X（米；Y = X × g_params.z）
    float4 colorAlpha; // rgb = 颜色，a = alpha（CPU 端按寿命曲线算好）
};

StructuredBuffer<ParticleGpu> g_particles : register(t0);
Texture2D       g_tex   : register(t1);
SamplerState    g_samp  : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

VSOutput vsMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // 顶点序 = 两枚三角形 (0,1,2)(3,4,5)，角点复用：
    //   0:(-1,-1) 1:(1,-1) 2:(1,1) 3:(-1,-1) 4:(1,1) 5:(-1,1)
    // 位式判定比数组常量在 slangc 双目标下更稳（避免 static 数组初始化差异）。
    const float cx = (vid == 1 || vid == 2 || vid == 4) ? 1.0f : -1.0f;
    const float cy = (vid == 2 || vid == 4 || vid == 5) ? 1.0f : -1.0f;
    const float2 corner = float2(cx, cy);

    ParticleGpu p = g_particles[iid];

    VSOutput output;
    // M-PT2：Y/X 尺寸比走 CB 标量 g_params.z（单发射器子集所有粒子同比例；
    // legacy/方形恒 1.0——cy*1.0f 逐位恒等 cy，M-PT0 表达式保形无 branch。
    // 教训：VS 里 if-else 会被 dxc select 化，动态 branch 混合评估致基线 ±1 LSB）。
    const float3 world = p.posSize.xyz
                       + (g_camRight.xyz * corner.x + g_camUp.xyz * (corner.y * g_params.z))
                         * p.posSize.w * g_params.x;
    output.position = mul(g_mvp, float4(world, 1.0f));
    output.uv       = corner;
    output.color    = p.colorAlpha;
    return output;
}

float4 psMain(VSOutput input) : SV_Target
{
    // 软圆盘：uv∈[-1,1]，d>1 的角落全透明（quad 角落不被丢弃，靠 alpha=0 融掉）。
    const float d = length(input.uv);
    const float disc = 1.0f - smoothstep(0.55f, 1.0f, d);

    // M-PT2：贴图采样分支保形——无贴图分支逐字保留 M-PT0 原表达式（uniform 分支
    // 全像素一致；DXIL 会把 lerp 展开成 FMA 造成 ±1 LSB 舍入，lerp-0「恒等」在
    // DX12 实测漂 15 像素，禁用）。uv∈[-1,1] → [0,1]²，Effekseer 口径 V0 = 贴图
    // 顶部映 billboard 顶部（camUp 正方向 = corner.y +1）→ v 轴翻转。
    if (g_params.y == 0.0f) {
        return float4(input.color.rgb, input.color.a * disc);
    }
    const float2 texUv = float2(input.uv.x * 0.5f + 0.5f, 0.5f - input.uv.y * 0.5f);
    const float4 tex = g_tex.Sample(g_samp, texUv);
    return float4(input.color.rgb * tex.rgb, input.color.a * tex.a);
}
