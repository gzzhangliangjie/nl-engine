// M-UI0.2 Ui 域示例脚本：C# 驱动 RmlUi 文档（改文本 + 事件回调 + 逐帧刷新）。
// 入口约定（tour 示例同口径）：Entry/OnTick 全部普通静态方法
//（BootCall/BootTickPtr 反射路径——.NET 8 禁反射 Invoke UC 方法，故不带 UC）。
// Entry（装载期）：挂事件回调（日志锚）+ 加载脚本文档（独立于 C++ demo 文档，
// panel 定位右上不重叠）+ ElementSetText 改 status（验收锚：C# 改文本可见）。
// OnTick（host 级逻辑帧）：每 60 tick 刷新 tick 行（动态数字只进 UI 不进 log，
// 保 log 字节对拍纪律）。
// 纪律：脚本无跨帧可变状态——_docHandle 仅 Entry 装载期一次写定；回调为静态
// 纯函数。热重载 close+reload 后 handle tombstone → rc=2 失效安全（不崩）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class UiScript
    {
        static int _docHandle;   // 0 = 未加载；tombstone 后 ElementSetText rc=2 失效安全

        // Entry：装载期三步锚定。返回 0 = 全链通过；非 0 = 失败步码。
        public static int Entry(IntPtr args, int size)
        {
            Engine.Init(args);

            // 1. 事件回调：id/type/param 全量日志锚（两份文档的按钮点击都走这里，
            //    C++ 侧每次现取 fn——热重载兜底，不缓存 delegate 指针）。
            int rc = Ui.SetEventCallback((elId, eventType, param) =>
                Log.Write(2, $"ui-event: id={elId} type={eventType} param={param}"));
            if (rc != 0) return 1;

            // 2. 加载脚本文档（FileInterface assets/ 双层解析；handle 由 C++ 发放）。
            _docHandle = Ui.LoadDocument("ui/scriptdoc.rml");
            if (_docHandle <= 0) return 2;

            // 3. C# 改文本（验收锚：右上 scriptdoc 面板 status 行可见变化）。
            rc = Ui.ElementSetText(_docHandle, "status", "text set from C# (Entry)");
            if (rc != 0) return 3;

            if (Log.Write(2, "ui-script: entry ok (event callback + doc loaded + text set)")
                != LogCode.Ok) return 4;
            return 0;
        }

        // OnTick：host 级逻辑帧驱动。契约（orbit 示例同款）：args = 指向
        // double dt（逻辑步长秒），size = 8；不调 Engine.Init（args 不是
        // ApiTable*——把 dt 位型当表指针解引用 = AV，tour 首版实测教训）。
        public static unsafe int OnTick(IntPtr args, int size)
        {
            if (size < sizeof(double) || args == IntPtr.Zero) return 1;
            if (Time.GetState(out _, out _, out ulong tick, out _) != TimeCode.Ok) return 0;
            if (tick % 60 == 0 && _docHandle > 0)
                Ui.ElementSetText(_docHandle, "tick", $"alive, tick {tick} (C# OnTick)");
            return 0;
        }
    }
}
