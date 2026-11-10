# Effekseer core vendored（MIT，选型闸见 VENDOR.md）
# 接线范式照 imgui：显式列源，不用上游 CMakeLists（其引用父级宏 filterfolder/warnerror，
# 且需 Threads/选项定义）。Network 子集未入库（拉 winsock/flatbuffers，离线转换器用不上）
# ——源码清单 = 上游 Dev/Cpp/Effekseer/CMakeLists.txt effekseer_src 去除 4 条 Network/*.cpp。
add_library(nl_effekseer STATIC
    Effekseer/Effekseer.Color.cpp
    Effekseer/Effekseer.CoordinateSystem.cpp
    Effekseer/Effekseer.CurveLoader.cpp
    Effekseer/Effekseer.DefaultEffectLoader.cpp
    Effekseer/Effekseer.DefaultFile.cpp
    Effekseer/Effekseer.Effect.cpp
    Effekseer/Effekseer.EffectNode.cpp
    Effekseer/Effekseer.EffectNodeModel.cpp
    Effekseer/Effekseer.EffectNodeRibbon.cpp
    Effekseer/Effekseer.EffectNodeRing.cpp
    Effekseer/Effekseer.EffectNodeRoot.cpp
    Effekseer/Effekseer.EffectNodeSprite.cpp
    Effekseer/Effekseer.EffectNodeTrack.cpp
    Effekseer/Effekseer.FCurves.cpp
    Effekseer/Effekseer.Instance.cpp
    Effekseer/Effekseer.InstanceChunk.cpp
    Effekseer/Effekseer.InstanceContainer.cpp
    Effekseer/Effekseer.InstanceGlobal.cpp
    Effekseer/Effekseer.InstanceGroup.cpp
    Effekseer/Effekseer.InternalScript.cpp
    Effekseer/Effekseer.IntrusiveList.cpp
    Effekseer/Effekseer.Manager.cpp
    Effekseer/Effekseer.Matrix43.cpp
    Effekseer/Effekseer.Matrix44.cpp
    Effekseer/Effekseer.Random.cpp
    Effekseer/Effekseer.RectF.cpp
    Effekseer/Effekseer.RenderingTransform.cpp
    Effekseer/Effekseer.Resource.cpp
    Effekseer/Effekseer.ResourceManager.cpp
    Effekseer/Effekseer.Setting.cpp
    Effekseer/Effekseer.Vector2D.cpp
    Effekseer/Effekseer.Vector3D.cpp
    Effekseer/Effekseer.WorkerThread.cpp
    Effekseer/ForceField/Effekseer.ForceFields.cpp
    Effekseer/IO/Effekseer.EfkEfcFactory.cpp
    Effekseer/Material/Effekseer.CompiledMaterial.cpp
    Effekseer/Material/Effekseer.MaterialCompiler.cpp
    Effekseer/Material/Effekseer.MaterialFile.cpp
    Effekseer/Model/Effekseer.Model.cpp
    Effekseer/Model/Effekseer.ModelLoader.cpp
    Effekseer/Model/Effekseer.PointCacheGenerator.cpp
    Effekseer/Model/Effekseer.ProceduralModelGenerator.cpp
    Effekseer/Model/Effekseer.SplineGenerator.cpp
    Effekseer/Noise/Effekseer.CurlNoise.cpp
    Effekseer/Parameter/Effekseer.Collisions.cpp
    Effekseer/Parameter/Effekseer.Easing.cpp
    Effekseer/Parameter/Effekseer.GpuParticlesParameter.cpp
    Effekseer/Parameter/Effekseer.KillRules.cpp
    Effekseer/Parameter/Effekseer.Parameters.cpp
    Effekseer/Parameter/Effekseer.Rotation.cpp
    Effekseer/Parameter/Effekseer.Trigger.cpp
    Effekseer/SIMD/Mat43f.cpp
    Effekseer/SIMD/Mat44f.cpp
    Effekseer/SIMD/Utils.cpp
    Effekseer/Utils/Effekseer.CustomAllocator.cpp
    Effekseer/VectorField/Effekseer.VectorField.cpp
    Effekseer/VectorField/Effekseer.VectorFieldLoader.cpp
)

target_include_directories(nl_effekseer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
