// M-W2A 验收脚本：Audio 域绑定面（selftest s16 节逐个调用）。
//
// 入口约定与 mouse 示例同口径：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 宿主注入（main.cpp s16）：selftest 自建 offline AudioWorld（spatialization 开，
// 无设备确定性）→ audioSetWorld 注入；资产 = assets/audio/test-tone.wav（既有）。
// 失效安全判据本体：未绑 AudioWorld 全族回 NoWorld；坏路径白名单拒绝（BadArg）；
// 缺文件 SourceFailed；未知句柄 Invalid——不抛异常不崩。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class AudioScript
    {
        // s16a：绑定路径全链断言。返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 播放复合链（create->configure->position->start）：id>0 + 播放态真
            if (Audio.Play("test-tone.wav", new float[] { 1f, 2f, 3f }, true, out int id)
                    != AudioCode.Ok || id <= 0) return 1;
            if (Audio.IsPlaying(id, out bool playing) != AudioCode.Ok || !playing) return 2;

            // 播放中控制：增益 / 空间位置 / 监听者位姿
            if (Audio.SetGain(id, 0.5f) != AudioCode.Ok) return 3;
            if (Audio.SetSourcePosition(id, new float[] { 4f, 5f, 6f }) != AudioCode.Ok) return 4;
            if (Audio.SetListenerPose(new float[] { 0f, 0f, 0f }, new float[] { 0f, 0f, -1f })
                    != AudioCode.Ok) return 5;

            // 停止 + 播放态翻假
            if (Audio.Stop(id) != AudioCode.Ok) return 6;
            if (Audio.IsPlaying(id, out bool playing2) != AudioCode.Ok || playing2) return 7;

            // 负向：路径穿越形态（带分隔符）白名单拒绝 = BadArg
            if (Audio.Play("sub/dir/bad.wav", new float[] { 0f, 0f, 0f }, false, out _)
                    != AudioCode.BadArg) return 8;
            // 负向：白名单过但文件缺失 = SourceFailed
            if (Audio.Play("no-such-clip.wav", new float[] { 0f, 0f, 0f }, false, out _)
                    != AudioCode.SourceFailed) return 9;
            // 负向：未知句柄 = Invalid（失效安全）
            if (Audio.Stop(999999) != AudioCode.Invalid) return 10;
            if (Audio.SetGain(999999, 1f) != AudioCode.Invalid) return 11;
            if (Audio.IsPlaying(999999, out _) != AudioCode.Invalid) return 12;

            return 0;
        }

        // s16b：解绑路径（宿主先 audioSetWorld(nullptr)）。
        // 判据：全族回 NoWorld（不隐式建表口径）。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Audio.Play("test-tone.wav", new float[] { 0f, 0f, 0f }, false, out _)
                    != AudioCode.NoWorld) return 1;
            if (Audio.Stop(1) != AudioCode.NoWorld) return 2;
            if (Audio.SetGain(1, 1f) != AudioCode.NoWorld) return 3;
            if (Audio.SetSourcePosition(1, new float[] { 0f, 0f, 0f }) != AudioCode.NoWorld) return 4;
            if (Audio.SetListenerPose(new float[] { 0f, 0f, 0f }, new float[] { 0f, 0f, -1f })
                    != AudioCode.NoWorld) return 5;
            if (Audio.IsPlaying(1, out _) != AudioCode.NoWorld) return 6;

            return 0;
        }
    }
}
