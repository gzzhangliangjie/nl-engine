// M8.1 (S8.1): TLAS closed-loop probe — inline ray query against the per-frame
// rebuilt TLAS (S8-PLAN M8.1 self-check: "move an instance's transform, the ray
// result follows next frame"). One fixed probe ray (horizontal sweep through
// model space); writes the committed hit t (float bits) to u0 for CPU readback
// logging. With --tlas-spin the TLAS instance rotates every frame, so t
// oscillates in the log = direct evidence that per-frame instance transforms
// propagate into ray results. Visual debug-ray overlay is M8.2 scope.
//
// Registers (docs/SHADER-CONVENTIONS.md):
//   t0 = TLAS (NVRHI BindingSetItem::RayTracingAccelStruct slot 0; DXIL t-space)
//   u0 = probe output (4B, read back 2 frames later like the cull counter)
// Vulkan: u0 maps through slangc -fvk-u-shift 384 0 (same as cull.slang).

RaytracingAccelerationStructure g_tlas     : register(t0);
RWStructuredBuffer<uint>        g_probeOut : register(u0);

[numthreads(1, 1, 1)]
void csTlasProbe(uint3 tid : SV_DispatchThreadID)
{
    RayDesc ray;
    ray.Origin    = float3(3.0, 0.2, 0.0);
    ray.Direction = normalize(float3(-1.0, 0.0, 0.0));
    ray.TMin      = 0.001;
    ray.TMax      = 100.0;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed();

    float t = (q.CommittedStatus() == COMMITTED_NOTHING) ? -1.0f
                                                         : q.CommittedRayT();
    g_probeOut[0] = asuint(t);
}
