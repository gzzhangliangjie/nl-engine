// P8 M-S2: wire audio.debug CVars (audio.volume / audio.debug / audio.solo /
// audio.freeze) and the audio.dump command into nl::core::console.
//
// This TU depends on the console (nl::core::console) so the rest of nl/audio
// (AudioWorld / AudioDebugController) stays graphics- and console-free. The
// viewer calls registerAudioConsole() alongside registerViewerConsole(); the
// audio-tools harness calls it too, so the CVar registration is exercised
// end-to-end by the headless tool cards.
#include "nl/audio/DebugController.hpp"
#include "nl/audio/AudioWorld.hpp"

#include "nl/core/console.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace nl::audio {

void registerAudioConsole(nl::core::console::Console& c) {
    using namespace nl::core::console;
    auto& ctrl = AudioDebugController::get();

    CVar& vol  = c.registerFloat("audio.volume", 1.0,
                                 "master audio volume multiplier (0..2)");
    CVar& dbg  = c.registerBool ("audio.debug", false,
                                 "show the in-game audio source debug panel");
    CVar& solo = c.registerInt  ("audio.solo", 0,
                                 "solo a source id (>0); <=0 clears solo");
    CVar& frz  = c.registerBool ("audio.freeze", false,
                                 "freeze the audio mix (single-step with audio.dump/step)");

    vol.setOnChanged ([&ctrl](const CVar& cv) { ctrl.setMasterVolume(static_cast<float>(cv.value().f)); });
    dbg.setOnChanged ([&ctrl](const CVar& cv) { ctrl.setDebug(cv.value().b); });
    solo.setOnChanged([&ctrl](const CVar& cv) { ctrl.setSolo(cv.value().i); });
    frz.setOnChanged ([&ctrl](const CVar& cv) { ctrl.setFrozen(cv.value().b); });

    c.registerCommand("audio.dump", "dump active audio sources + the live mix to <path>",
        [&ctrl](const std::vector<std::string>& args) {
            if (args.empty())
                return CommandResult{false, "usage: audio.dump <path>"};
            auto* w = ctrl.bound();
            if (!w)
                return CommandResult{false, "audio.dump: no active audio world bound"};
            const std::string text = ctrl.debugText();
            std::ofstream f(args[0], std::ios::binary | std::ios::trunc);
            if (!f)
                return CommandResult{false, "audio.dump: cannot write " + args[0]};
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!f)
                return CommandResult{false, "audio.dump: write failed " + args[0]};
            const auto snap = w->snapshot();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu",
                          static_cast<std::size_t>(snap.size()));
            return CommandResult{true, std::string("audio.dump: wrote ") + buf +
                                      " source(s) to " + args[0]};
        });
}

} // namespace nl::audio
