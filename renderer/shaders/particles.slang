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
//   b0 = ParticleConstantsCB（volatile，每帧全量覆写，128 B）
//   t0 = StructuredBuffer<ParticleGpu>（每实例 64 B：posSize + colorAlpha +
//        progressMisc(z=angle,w=sizeY比) + misc2 spare）
//   t1 = 粒子贴图（nfx texture 字段；无贴图绑 1x1 白 dummy）
//   s0 = Linear clamp 采样器（M-PT3：mip 线性，纹理带全 mip 链）
//   g_params.y = 贴图开关（0 = 软圆盘路径，1 = 贴图采样）
//   g_params.z = [M-PT5 退役] Y/X 尺寸比改 per-instance progressMisc.w；
//                CB 布局不动（z 仍写 ratioY 但 shader 不再读）
//   g_params.w = flipbook 开关（0 = 恒等 UV，1 = 序列帧 cell 采样）
//   g_uvRect   = UV 区域（xy = offset，zw = cell 尺寸；非 flipbook = (0,0,1,1)）
//
// 布局铁律：结构体与 scene.cpp 的 ParticleGpu / ParticleConstantsCB 逐字节
// 对应（48 B / 128 B），漂移会直接画花而不是报错——改任何一侧必须同步另一侧。

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
    // 预留（当前 x=整体尺寸倍率）。
    float4 g_params;
    // M-PT3：flipbook UV 区域。非 flipbook 恒 (0,0,1,1)——此时 flipUv 公式
    // 数学恒等于 M-PT2 的 texUv（×1.0 / +0.0 精确恒等，无 branch）。
    float4 g_uvRect;
};

struct ParticleGpu
{
    float4 posSize;      // xyz = 世界位置，w = 尺寸 X（米；Y = X × progressMisc.w）
    float4 colorAlpha;   // rgb = 颜色，a = alpha（CPU 端按寿命曲线算好）
    float4 progressMisc; // xy = flipbook 帧号 (frameX, frameY)（CPU 折算，非
                         // flipbook = (0,0)）；M-PT5：z = 角度（弧度，无旋转
                         // 恒 0），w = 该粒子 Y/X 尺寸比
    float4 misc2;        // M-PT5 spare（E3/E4/E8 未来字段，恒 0）
};

StructuredBuffer<ParticleGpu> g_particles : register(t0);
Texture2D       g_tex   : register(t1);
SamplerState    g_samp  : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;   // corner ∈ [-1,1]²（软圆盘距离场）
    float2 flipUv   : TEXCOORD1;   // M-PT3：纹理 UV（贴图路径采样用）
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
    // M-PT5 R1：Y/X 尺寸比从 CB 标量 g_params.z 改 per-instance 读（R1 随机
    // sizeY per-particle，拍板 Q3=b）。定值路径：CPU spawn 端逐粒子复制
    // params_.ratioY 同一 float → cy*w 与旧 cy*g_params.z 逐位同。
    // M-PT5 E5：billboard 屏幕平面旋转（2D 旋转矩阵作用 corner，无 branch）。
    // 保形路线（M-PT2/M-PT3 先例）：angle=0 → cos(0)=1.0、sin(0)=0.0 精确，
    // rc = (cx*1 - cy*0, cx*0 + cy*1) = (cx, cy) IEEE 逐位恒等（corner ∈ ±1，
    // 无 ±0 舍入歧途）；旋转只作用于几何展开角点，output.uv/flipUv 保持原
    // corner（贴图绑定 quad 局部坐标 → 贴图随 quad 转；软圆盘径向对称）。
    const float ca = cos(p.progressMisc.z);
    const float sa = sin(p.progressMisc.z);
    const float2 rc = float2(corner.x * ca - corner.y * sa,
                             corner.x * sa + corner.y * ca);
    const float3 world = p.posSize.xyz
                       + (g_camRight.xyz * rc.x + g_camUp.xyz * (rc.y * p.progressMisc.w))
                         * p.posSize.w * g_params.x;
    output.position = mul(g_mvp, float4(world, 1.0f));
    output.uv       = corner;
    output.color    = p.colorAlpha;
    // M-PT3：统一 flipbook UV 公式（无 branch）。非 flipbook：g_uvRect=(0,0,1,1)、
    // progressMisc.xy=(0,0) → flipUv = localUv = M-PT2 的 texUv 逐位恒等；
    // flipbook：CPU 已折算帧号 → origin = offset + cell*frame，cell 内插值。
    // V 轴口径：Effekseer V0 = 贴图顶部映 billboard 顶部（corner.y=+1）→ v 翻转。
    const float2 localUv = float2(corner.x * 0.5f + 0.5f, 0.5f - corner.y * 0.5f);
    output.flipUv = g_uvRect.xy + g_uvRect.zw * (p.progressMisc.xy + localUv);
    return output;
}

float4 psMain(VSOutput input) : SV_Target
{
    // 软圆盘：uv∈[-1,1]，d>1 的角落全透明（quad 角落不被丢弃，靠 alpha=0 融掉）。
    const float d = length(input.uv);
    const float disc = 1.0f - smoothstep(0.55f, 1.0f, d);

    // M-PT2：贴图采样分支保形——无贴图分支逐字保留 M-PT0 原表达式（uniform 分支
    // 全像素一致；DXIL 会把 lerp 展开成 FMA 造成 ±1 LSB 舍入，lerp-0「恒等」在
    // DX12 实测漂 15 像素，禁用）。M-PT3：texUv 计算移到 VS 输出 flipUv（恒等
    // 移动，非 flipbook 路径像素结果逐位同；软圆盘分支不读 flipUv 零影响）。
    if (g_params.y == 0.0f) {
        return float4(input.color.rgb, input.color.a * disc);
    }
    const float4 tex = g_tex.Sample(g_samp, input.flipUv);
    return float4(input.color.rgb * tex.rgb, input.color.a * tex.a);
}
