// CLAP glue: entry point, factory, plugin, audio/note ports, and GUI.
// Structure ported from NeoLooper's plugin.cpp (sibling project), adapted
// for an instrument (audio output only, MIDI note input) instead of an
// effect. No params extension - see project memory/plan for why (matches
// NeoLooper: every control is a plain atomic, no host automation lane).
#include <clap/clap.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>

#include "gui/editor_ui.hpp"
#include "gui/gui_window.hpp"
#include "gui/zenity_dialog.h"
#include "imgui.h"
#include "sfizz/SfizzEngine.h"
#include "shared.hpp"
#include "state/PresetFile.h"
#include "state/SampleInfo.h"
#include "state/SfzDocument.h"
#include "state/SfzFlatten.h"

namespace {

constexpr uint32_t kDefaultW = 560, kDefaultH = 680;

struct Plugin {
    clap_plugin_t plug{};
    const clap_host_t* host = nullptr;
    std::unique_ptr<GuiWindow> window;
    SfizzEngine engine;
    SharedParams params;
    EditorUIState uiState;

    explicit Plugin(const clap_host_t* h) : host(h) {}
};

// Rebuilds the SFZ text from the current params/sample path and reloads it
// into the engine. CT+OFF (see SfizzEngine.h) - call only from a non-audio
// thread (activation, or the GUI thread once a sample is loaded/changed).
void regenerateAndLoadSfz(Plugin* p) {
    SfzRegionParams rp;
    int64_t numFrames = 0;
    std::string scalaFilePath;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        rp.stack.reserve(p->params.guiState.stack.size());
        for (const auto& item : p->params.guiState.stack) {
            SfzRegionParams::StackRegion region;
            region.isSfz = item.isSfz;
            region.sampleRelativePath = item.sampleRelativePath;
            region.regionsText = item.regionsText;
            rp.stack.push_back(std::move(region));
        }
        numFrames = p->params.guiState.numFrames;
        rp.customOpcodes = p->params.guiState.customOpcodesText;
        scalaFilePath = p->params.guiState.scalaFilePath;
    }
    rp.character = p->params.character.load();
    rp.multisampleRootNote = p->params.multisampleRootNote.load();
    rp.panX2 = p->params.panX2.load();
    rp.bendUpCents = p->params.bendUpCents.load();
    rp.bendDownCents = p->params.bendDownCents.load();
    rp.quality = p->params.quality.load();
    rp.loopMode = kLoopModes[p->params.loopModeIndex.load()];
    rp.polyphony = p->params.polyphony.load();
    rp.notePolyphony = p->params.notePolyphony.load();
    rp.tuneCents = p->params.tuneCents.load();
    rp.rootNote = p->params.rootNote.load();
    rp.volume = p->params.volume.load();
    rp.offsetEnabled = p->params.offsetEnabled.load();
    rp.offsetValue = p->params.offsetValue.load();
    rp.randomOffsetEnabled = p->params.randomOffsetEnabled.load();
    // random_offset = total samples - offset (less offset headroom left as
    // the user pushes the fixed offset further in) - recomputed fresh every
    // regeneration, never stored, so it can't go stale.
    {
        const int maxOffset = static_cast<int>(
            numFrames > 0 ? numFrames : SampleInfo::defaultMaxOffset);
        rp.randomOffsetValue = std::max(0, maxOffset - rp.offsetValue);
    }

    rp.panRandom = p->params.panRandom.load();
    rp.panAlternate = p->params.panAlternate.load();
    rp.panLfoDelay = p->params.panLfoDelay.load();
    rp.panLfoFade = p->params.panLfoFade.load();
    rp.panLfoPan = p->params.panLfoPan.load();
    rp.panLfoFreq = p->params.panLfoFreq.load();
    rp.panLfoWave = p->params.panLfoWave.load();

    rp.ampKeycenter = p->params.ampKeycenter.load();
    rp.ampKeytrack = p->params.ampKeytrack.load();
    rp.ampVeltrack = p->params.ampVeltrack.load();
    rp.ampRandom = p->params.ampRandom.load();
    rp.ampStartLevel = p->params.ampStartLevel.load();
    rp.ampDelayTime = p->params.ampDelayTime.load();
    rp.ampAttackTime = p->params.ampAttackTime.load();
    rp.ampAttackShape = p->params.ampAttackShape.load();
    rp.ampHoldTime = p->params.ampHoldTime.load();
    rp.ampDecayTime = p->params.ampDecayTime.load();
    rp.ampDecayShape = p->params.ampDecayShape.load();
    rp.ampSustainLevel = p->params.ampSustainLevel.load();
    rp.ampReleaseTime = p->params.ampReleaseTime.load();
    rp.ampReleaseShape = p->params.ampReleaseShape.load();
    rp.ampLfoDelay = p->params.ampLfoDelay.load();
    rp.ampLfoFade = p->params.ampLfoFade.load();
    rp.ampLfoVolume = p->params.ampLfoVolume.load();
    rp.ampLfoFreq = p->params.ampLfoFreq.load();
    rp.ampLfoWave = p->params.ampLfoWave.load();

    rp.filterType = kFilterTypes[p->params.filterTypeIndex.load()];
    rp.filterCutoff = p->params.filterCutoff.load();
    rp.filterResonance = p->params.filterResonance.load();
    rp.filterRandomCutoff = p->params.filterRandomCutoff.load();
    rp.filKeycenter = p->params.filKeycenter.load();
    rp.filKeytrack = p->params.filKeytrack.load();
    rp.filVeltrack = p->params.filVeltrack.load();
    rp.resoVeltrack = p->params.resoVeltrack.load();
    rp.filterEgEnabled = p->params.filterEgEnabled.load();
    rp.filDepth = p->params.filDepth.load();
    rp.filEgStartLevel = p->params.filEgStartLevel.load();
    rp.filEgDelayTime = p->params.filEgDelayTime.load();
    rp.filEgAttackTime = p->params.filEgAttackTime.load();
    rp.filEgAttackShape = p->params.filEgAttackShape.load();
    rp.filEgHoldTime = p->params.filEgHoldTime.load();
    rp.filEgDecayTime = p->params.filEgDecayTime.load();
    rp.filEgDecayShape = p->params.filEgDecayShape.load();
    rp.filEgSustainLevel = p->params.filEgSustainLevel.load();
    rp.filEgReleaseTime = p->params.filEgReleaseTime.load();
    rp.filEgReleaseShape = p->params.filEgReleaseShape.load();
    rp.filterLfoDelay = p->params.filterLfoDelay.load();
    rp.filterLfoFade = p->params.filterLfoFade.load();
    rp.filterLfoDepth = p->params.filterLfoDepth.load();
    rp.filterLfoFreq = p->params.filterLfoFreq.load();
    rp.filterLfoWave = p->params.filterLfoWave.load();

    rp.pitchKeytrack = p->params.pitchKeytrack.load();
    rp.pitchVeltrack = p->params.pitchVeltrack.load();
    rp.pitchRandom = p->params.pitchRandom.load();
    rp.portamentoEnabled = p->params.portamentoEnabled.load();
    rp.glideTime = p->params.glideTime.load();
    rp.pitchEgEnabled = p->params.pitchEgEnabled.load();
    rp.pitchDepth = p->params.pitchDepth.load();
    rp.pitchEgStartLevel = p->params.pitchEgStartLevel.load();
    rp.pitchEgDelayTime = p->params.pitchEgDelayTime.load();
    rp.pitchEgAttackTime = p->params.pitchEgAttackTime.load();
    rp.pitchEgAttackShape = p->params.pitchEgAttackShape.load();
    rp.pitchEgHoldTime = p->params.pitchEgHoldTime.load();
    rp.pitchEgDecayTime = p->params.pitchEgDecayTime.load();
    rp.pitchEgDecayShape = p->params.pitchEgDecayShape.load();
    rp.pitchEgSustainLevel = p->params.pitchEgSustainLevel.load();
    rp.pitchEgReleaseTime = p->params.pitchEgReleaseTime.load();
    rp.pitchEgReleaseShape = p->params.pitchEgReleaseShape.load();
    rp.pitchLfoDelay = p->params.pitchLfoDelay.load();
    rp.pitchLfoFade = p->params.pitchLfoFade.load();
    rp.pitchLfoPitch = p->params.pitchLfoPitch.load();
    rp.pitchLfoFreq = p->params.pitchLfoFreq.load();
    rp.pitchLfoWave = p->params.pitchLfoWave.load();

    rp.fxEnabled = p->params.fxEnabled.load();
    rp.fxMode = p->params.fxMode.load();
    rp.fxDetune = p->params.fxDetune.load();
    rp.fxDetuneCcMode = p->params.fxDetuneCcMode.load();
    rp.fxDelay = p->params.fxDelay.load();
    rp.fxDelayCcMode = p->params.fxDelayCcMode.load();
    rp.fxStereoWidth = p->params.fxStereoWidth.load();
    rp.fxDepth = p->params.fxDepth.load();
    rp.fxSpeed = p->params.fxSpeed.load();
    rp.fxWave = p->params.fxWave.load();
    rp.fxPhase = p->params.fxPhase.load();
    rp.fxPhaseCcMode = p->params.fxPhaseCcMode.load();
    rp.fxIndependentLfo = p->params.fxIndependentLfo.load();

    rp.fil2Type = kFilterTypes[p->params.fil2TypeIndex.load()];
    rp.cutoff2 = p->params.cutoff2.load();
    rp.fxVolume = p->params.fxVolume.load();

    rp.reverbEnabled = p->params.reverbEnabled.load();
    rp.reverbType = kReverbTypeNames[p->params.reverbTypeIndex.load()];
    rp.reverbInput = p->params.reverbInput.load();
    rp.reverbPredelay = p->params.reverbPredelay.load();
    rp.reverbSize = p->params.reverbSize.load();
    rp.reverbTone = p->params.reverbTone.load();
    rp.reverbDamp = p->params.reverbDamp.load();
    rp.reverbDry = p->params.reverbDry.load();
    rp.reverbWet = p->params.reverbWet.load();

    std::string text = buildSfzText(rp);
    p->engine.loadSfzString("/", text);

    // Tuning is a synth-level setting, not part of the SFZ text just
    // reloaded above - re-apply it every time in case reloading the SFZ
    // would otherwise reset it (see SfizzEngine::loadScalaFile's comment).
    // Failure here is advisory only (guiState.scalaError) since this can
    // run on every single param tweak, not just an explicit user pick.
    bool scalaOk = scalaFilePath.empty() || p->engine.loadScalaFile(scalaFilePath);

    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    p->params.guiState.lastSfzText = std::move(text);
    p->params.guiState.scalaError =
        scalaOk ? std::string() : ("Could not load scala file: " + scalaFilePath);
}

// Root-relative sample= value for a plain sample: the virtual sfz "path"
// sfizz is told about is always "/" (see regenerateAndLoadSfz's
// loadSfzString call), so sample= must carry the file's full absolute path
// with the leading '/' stripped, not a path relative to some other fake
// root.
std::string sampleRelativePath(const std::string& absPath) {
    return (!absPath.empty() && absPath.front() == '/') ? absPath.substr(1) : absPath;
}

// Loads a full batch of picked/dropped files, REPLACING the Sample tab's
// whole stack in one shot - the single entry point both the file dialog
// (single pick, or a Ctrl+click multi-selection - see file_dialog.hpp) and
// XDND drag-and-drop (one drop's whole file list) funnel through. A pick of
// exactly one file behaves like loading always did before the stack feature
// existed (replaces whatever was loaded); two or more replace it with all of
// them, layered together. The stack is never built up by separate calls -
// only by one explicit multi-file selection/drop (see project spec). A
// plain sample becomes a single bare region; a .sfz is run through
// flattenMultisampleSfz() (resolves its own #define/#include/default_path/
// cascade, rewrites every sample= relative to SoloSampler's virtual "/"
// root) into its own pre-flattened region block - buildSfzText concatenates
// every stacked item so they all play together (see shared.hpp's
// GuiState::StackItem). GUI thread only. Files this plugin can't use, or an
// .sfz that fails to flatten, are silently skipped as long as at least one
// other path in the batch succeeds (the skip is reported via
// guiState.stackError, advisory); returns false (nothing mutated) only if
// NONE of the given paths were usable.
bool loadStack(Plugin* p, const std::vector<std::string>& paths) {
    std::vector<SharedParams::GuiState::StackItem> items;
    items.reserve(paths.size());
    std::string firstError;
    SampleInfo soleInfo; // only meaningful once the loop below ends with items.size()==1 && !isSfz

    for (const auto& path : paths) {
        const bool isSfz = isSfzFile(path);
        if (!isSfz && !isSupportedAudioFile(path)) continue;

        SharedParams::GuiState::StackItem item;
        item.path = path;
        item.isSfz = isSfz;

        if (isSfz) {
            FlattenedSfz flat = flattenMultisampleSfz(path);
            if (!flat.ok) {
                if (firstError.empty()) firstError = flat.error;
                continue;
            }
            item.regionsText = flat.regionsText;
            item.regionCount = flat.regionCount;
        } else {
            SampleInfo info = analyzeSampleFile(path);
            if (!info.isValid) {
                if (firstError.empty()) firstError = "Could not load sample: " + path;
                continue;
            }
            item.sampleRelativePath = sampleRelativePath(path);
            item.regionCount = 1;
            soleInfo = info;
        }
        items.push_back(std::move(item));
    }

    if (items.empty()) {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.stackError = firstError.empty() ? "No usable files" : firstError;
        return false;
    }

    const bool soleIsSample = items.size() == 1 && !items[0].isSfz;
    const bool soleIsSfz = items.size() == 1 && items[0].isSfz;

    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.stack = items;
        p->params.guiState.stackError = firstError;
        p->params.guiState.lastBrowseDir =
            std::filesystem::path(paths.back()).parent_path().string();
        // Only a lone plain sample has a real frame count/waveform to show
        // - see drawSampleTab. Any other situation (a lone .sfz, or 2+
        // items of any mix) falls back to "no sample"/the fixed default
        // offset range.
        if (soleIsSample) {
            p->params.guiState.numFrames = soleInfo.numFrames;
            p->params.guiState.loopStartFrame =
                soleInfo.hasLoopPoints ? soleInfo.loopStartFrame : -1;
            p->params.guiState.loopEndFrame = soleInfo.hasLoopPoints ? soleInfo.loopEndFrame : -1;
        } else {
            p->params.guiState.numFrames = 0;
            p->params.guiState.loopStartFrame = -1;
            p->params.guiState.loopEndFrame = -1;
        }
    }

    if (soleIsSample) {
        if (soleInfo.hasRootNote) {
            p->params.rootNote = std::clamp(soleInfo.rootNote, 0, 127);
            p->params.tuneCents = std::clamp(soleInfo.fineTuneCents, -100.0f, 100.0f);
        }
        if (soleInfo.hasLoopPoints)
            p->params.loopModeIndex = 2; // "loop_continuous"
        const int maxOffset = static_cast<int>(soleInfo.maxOffset());
        if (p->params.offsetValue.load() > maxOffset) p->params.offsetValue = maxOffset;
        p->uiState.waveform.load(items[0].path);
    } else {
        if (soleIsSfz)
            // Every fresh import starts the piano's right-click nudge
            // neutral - see SharedParams::multisampleRootNote.
            p->params.multisampleRootNote = 60;
        p->uiState.waveform.clear();
    }

    regenerateAndLoadSfz(p);
    return true;
}

// Removes stack[index] (Sample tab file-list's "x" button) and re-derives
// the waveform/offset display for whatever's left, same rules loadStack
// uses (a lone remaining plain sample gets its waveform back; anything else
// shows no waveform/the default offset range). GUI thread only.
void removeStackItem(Plugin* p, size_t index) {
    std::string solePath;
    bool soleIsSample = false;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& stack = p->params.guiState.stack;
        if (index >= stack.size()) return;
        stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index));
        p->params.guiState.stackError.clear();
        if (stack.size() == 1 && !stack[0].isSfz) {
            solePath = stack[0].path;
            soleIsSample = true;
        } else {
            p->params.guiState.numFrames = 0;
            p->params.guiState.loopStartFrame = -1;
            p->params.guiState.loopEndFrame = -1;
        }
    }

    if (soleIsSample) {
        SampleInfo info = analyzeSampleFile(solePath);
        {
            std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
            p->params.guiState.numFrames = info.isValid ? info.numFrames : 0;
            p->params.guiState.loopStartFrame =
                info.isValid && info.hasLoopPoints ? info.loopStartFrame : -1;
            p->params.guiState.loopEndFrame =
                info.isValid && info.hasLoopPoints ? info.loopEndFrame : -1;
        }
        p->uiState.waveform.load(solePath);
    } else {
        p->uiState.waveform.clear();
    }

    regenerateAndLoadSfz(p);
}

// Empties the whole stack (Sample tab's "Clear All" button). GUI thread only.
void clearStack(Plugin* p) {
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.stack.clear();
        p->params.guiState.numFrames = 0;
        p->params.guiState.loopStartFrame = -1;
        p->params.guiState.loopEndFrame = -1;
        p->params.guiState.stackError.clear();
    }
    p->uiState.waveform.clear();
    regenerateAndLoadSfz(p);
}

// Rebuilds guiState.stack from a persisted list of stack entries (CLAP host
// state / .sspreset restore) - unlike loadStack() (a fresh user pick), this
// trusts the persisted data as-is (an .sfz's cached regionsText, a
// plain sample's path) rather than re-deriving tuning from it, since
// rootNote/tuneCents/loopModeIndex were already restored separately by
// applyPresetFields' caller. A missing source file is advisory only
// (guiState.stackError) - a cached SFZ mapping/sample= opcode still works
// even if its source moved, same as before this feature. GUI thread only.
void restoreStack(Plugin* p, const std::vector<PresetStackItem>& saved) {
    std::vector<SharedParams::GuiState::StackItem> items;
    items.reserve(saved.size());
    std::string firstMissing;
    for (const auto& s : saved) {
        SharedParams::GuiState::StackItem item;
        item.path = s.path;
        item.isSfz = s.isSfz;
        if (s.isSfz) {
            item.regionsText = s.regionsText;
            item.regionCount = 0;
            for (size_t pos = s.regionsText.find("<region>"); pos != std::string::npos;
                pos = s.regionsText.find("<region>", pos + 1))
                ++item.regionCount;
        } else {
            item.sampleRelativePath = sampleRelativePath(s.path);
            item.regionCount = 1;
        }
        if (!s.path.empty() && firstMissing.empty() && !std::filesystem::is_regular_file(s.path))
            firstMissing = s.path;
        items.push_back(std::move(item));
    }

    const bool soleIsSample = items.size() == 1 && !items[0].isSfz;
    const std::string solePath = soleIsSample ? items[0].path : std::string();
    SampleInfo info; // only meaningful when soleIsSample
    if (soleIsSample) info = analyzeSampleFile(solePath);

    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.stack = std::move(items);
        p->params.guiState.numFrames = soleIsSample && info.isValid ? info.numFrames : 0;
        p->params.guiState.loopStartFrame =
            soleIsSample && info.isValid && info.hasLoopPoints ? info.loopStartFrame : -1;
        p->params.guiState.loopEndFrame =
            soleIsSample && info.isValid && info.hasLoopPoints ? info.loopEndFrame : -1;
        p->params.guiState.stackError =
            firstMissing.empty() ? std::string() : ("File not found: " + firstMissing);
    }
    if (soleIsSample && info.isValid) p->uiState.waveform.load(solePath);
    else p->uiState.waveform.clear();
}

// Canonical 12-tone equal temperament Scala definition (the format's own
// standard reference example) - loaded via SfizzEngine::loadScalaString to
// put the engine back to plain, un-retuned standard tuning wherever "no
// scala file" needs to be a concrete, active state rather than just an
// absence (see restoreScala/plugin.cpp's Clear button handler).
constexpr const char* kStandardTuningScala =
    "! 12tet.scl\n"
    "!\n"
    "12 Tone Equal Temperament\n"
    " 12\n"
    "!\n"
    " 100.0\n"
    " 200.0\n"
    " 300.0\n"
    " 400.0\n"
    " 500.0\n"
    " 600.0\n"
    " 700.0\n"
    " 800.0\n"
    " 900.0\n"
    " 1000.0\n"
    " 1100.0\n"
    " 2/1\n";

// Restores the Pitch tab's Scala tuning from a saved CLAP host state (NOT
// part of .sspreset/.ssprofile - see SharedParams::tuningFrequency's own
// comment on why this is DAW-session-only). Unlike restoreStack(), a
// missing/failed scala file has no cached fallback data to keep working
// with, so this falls back to standard 12-TET (still a definite, correct
// tuning) rather than leaving stale/undefined state active - the path
// itself is still recorded (and the failure surfaced via
// guiState.scalaError) so the Pitch tab can show what it tried to restore.
// GUI thread only (CT+OFF, see SfizzEngine::loadScalaFile).
void restoreScala(Plugin* p, const std::string& path) {
    const bool ok = !path.empty() && p->engine.loadScalaFile(path);
    if (!ok) p->engine.loadScalaString(kStandardTuningScala);

    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    p->params.guiState.scalaFilePath = path;
    p->params.guiState.scalaError =
        (!path.empty() && !ok) ? ("Could not load scala file: " + path) : std::string();
}

// Load Scala File button handler (Pitch tab's Tuning section): picks a
// .scl file via zenity and applies it immediately. Cancelling zenity (empty
// path) is silent, not an error - a failed load leaves whatever tuning was
// already active untouched (same "don't mutate on failure" convention as
// loadStack's per-item failures).
void handleLoadScalaFile(Plugin* p) {
    std::string browseDir;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        browseDir = p->params.guiState.scalaBrowseDir;
        // First-ever open (or after a state restore) with nothing browsed
        // yet this session: start next to whichever scala file is already
        // loaded, if any, rather than wherever zenity defaults to.
        if (browseDir.empty() && !p->params.guiState.scalaFilePath.empty())
            browseDir = std::filesystem::path(p->params.guiState.scalaFilePath).parent_path().string();
    }

    std::string path = zenityOpenFile("Load Scala File", browseDir.empty() ? "" : browseDir + "/",
                                      {{"Scala Tuning File", "*.scl"}});
    if (path.empty()) return;

    bool ok = p->engine.loadScalaFile(path);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    if (ok) {
        p->params.guiState.scalaFilePath = path;
        p->params.guiState.scalaError.clear();
    } else {
        p->params.guiState.scalaError = "Could not load scala file: " + path;
    }
    // Remembered regardless of success/failure - even a failed pick tells us
    // which folder the user was browsing, still the best guess for next time.
    p->params.guiState.scalaBrowseDir = std::filesystem::path(path).parent_path().string();
}

// Clear button handler (Pitch tab's Tuning section): reverts to standard
// 12-TET tuning.
void handleClearScala(Plugin* p) {
    p->engine.loadScalaString(kStandardTuningScala);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    p->params.guiState.scalaFilePath.clear();
    p->params.guiState.scalaError.clear();
}

Plugin* self(const clap_plugin_t* p) {
    return static_cast<Plugin*>(p->plugin_data);
}

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                           CLAP_PLUGIN_FEATURE_SAMPLER,
                           CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDesc = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.sfzlab.solosampler",
    .name = "SoloSampler",
    .vendor = "michael02022",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.2.0",
    .description = "Single-region SFZ sampler backed by sfizz.",
    .features = kFeatures,
};

// ------------------------------------------------------------- audio ports

uint32_t audioPortsCount(const clap_plugin_t*, bool is_input) {
    return is_input ? 0 : 1;
}

bool audioPortsGet(const clap_plugin_t*, uint32_t index, bool is_input,
                   clap_audio_port_info_t* info) {
    if (is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "Output");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kExtAudioPorts = {
    .count = audioPortsCount,
    .get = audioPortsGet,
};

// -------------------------------------------------------------- note ports

uint32_t notePortsCount(const clap_plugin_t*, bool is_input) {
    return is_input ? 1 : 0;
}

bool notePortsGet(const clap_plugin_t*, uint32_t index, bool is_input,
                  clap_note_port_info_t* info) {
    if (!is_input || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    snprintf(info->name, sizeof(info->name), "MIDI In");
    return true;
}

const clap_plugin_note_ports_t kExtNotePorts = {
    .count = notePortsCount,
    .get = notePortsGet,
};

// ------------------------------------------------------------------- state
//
// Much simpler than NeoLooper's (no raw sample bytes to embed - just the
// sample's file path + the SharedParams atomics, matching what the old
// JUCE version's getStateInformation/setStateInformation already did). A
// small versioned binary blob, hand-rolled (no XML/JSON dependency).

constexpr uint32_t kStateMagic = 0x53534C50; // "PLSS" (SoloSampler) LE bytes
constexpr uint32_t kStateVersion = 22; // v22: tuningFrequency/scalaFilePath (DAW-session-only, not preset)

bool streamWriteAll(const clap_ostream_t* stream, const void* data, uint64_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t written = 0;
    while (written < size) {
        int64_t n = stream->write(stream, p + written, size - written);
        if (n <= 0) return false;
        written += static_cast<uint64_t>(n);
    }
    return true;
}

bool streamReadAll(const clap_istream_t* stream, void* data, uint64_t size) {
    uint8_t* p = static_cast<uint8_t*>(data);
    uint64_t readTotal = 0;
    while (readTotal < size) {
        int64_t n = stream->read(stream, p + readTotal, size - readTotal);
        if (n <= 0) return false;
        readTotal += static_cast<uint64_t>(n);
    }
    return true;
}

template <typename T>
bool writeVal(const clap_ostream_t* s, const T& v) {
    return streamWriteAll(s, &v, sizeof(T));
}
template <typename T>
bool readVal(const clap_istream_t* s, T& v) {
    return streamReadAll(s, &v, sizeof(T));
}

// Applies every field in f to the running plugin instance and regenerates
// the SFZ - shared by stateLoad (CLAP host state restore, always
// applySampleData=true) and loadSoloSamplerPreset (Preset/Profile files,
// see state/PresetFile.h) - applySampleData is false for Load Profile so
// the currently-loaded sample/multisample mapping is left completely
// alone, only the "instrument" fields below it change.
void applyPresetFields(Plugin* p, const PresetFields& f, bool applySampleData) {
    p->params.rootNote = f.rootNote;
    p->params.volume = f.volume;
    p->params.bendUpCents = f.bendUpCents;
    p->params.bendDownCents = f.bendDownCents;
    p->params.quality = f.quality;
    p->params.loopModeIndex = f.loopModeIndex;
    p->params.polyphony = f.polyphony;
    p->params.notePolyphony = f.notePolyphony;
    p->params.tuneCents = f.tuneCents;
    p->params.offsetEnabled = f.offsetEnabled;
    p->params.offsetValue = f.offsetValue;
    p->params.randomOffsetEnabled = f.randomOffsetEnabled;
    p->params.panRandom = f.panRandom;
    p->params.panAlternate = f.panAlternate;
    p->params.panLfoDelay = f.panLfoDelay;
    p->params.panLfoFade = f.panLfoFade;
    p->params.panLfoPan = f.panLfoPan;
    p->params.panLfoFreq = f.panLfoFreq;
    p->params.panLfoWave = f.panLfoWave;
    p->params.ampKeycenter = f.ampKeycenter;
    p->params.ampKeytrack = f.ampKeytrack;
    p->params.ampVeltrack = f.ampVeltrack;
    p->params.ampRandom = f.ampRandom;
    p->params.ampStartLevel = f.ampStartLevel;
    p->params.ampDelayTime = f.ampDelayTime;
    p->params.ampAttackTime = f.ampAttackTime;
    p->params.ampAttackShape = f.ampAttackShape;
    p->params.ampHoldTime = f.ampHoldTime;
    p->params.ampDecayTime = f.ampDecayTime;
    p->params.ampDecayShape = f.ampDecayShape;
    p->params.ampSustainLevel = f.ampSustainLevel;
    p->params.ampReleaseTime = f.ampReleaseTime;
    p->params.ampReleaseShape = f.ampReleaseShape;
    p->params.ampLfoDelay = f.ampLfoDelay;
    p->params.ampLfoFade = f.ampLfoFade;
    p->params.ampLfoVolume = f.ampLfoVolume;
    p->params.ampLfoFreq = f.ampLfoFreq;
    p->params.ampLfoWave = f.ampLfoWave;
    p->params.filterTypeIndex = f.filterTypeIndex;
    p->params.filterCutoff = f.filterCutoff;
    p->params.filterResonance = f.filterResonance;
    p->params.filterRandomCutoff = f.filterRandomCutoff;
    p->params.filKeycenter = f.filKeycenter;
    p->params.filKeytrack = f.filKeytrack;
    p->params.filVeltrack = f.filVeltrack;
    p->params.resoVeltrack = f.resoVeltrack;
    p->params.filterEgEnabled = f.filterEgEnabled;
    p->params.filDepth = f.filDepth;
    p->params.filEgStartLevel = f.filEgStartLevel;
    p->params.filEgDelayTime = f.filEgDelayTime;
    p->params.filEgAttackTime = f.filEgAttackTime;
    p->params.filEgAttackShape = f.filEgAttackShape;
    p->params.filEgHoldTime = f.filEgHoldTime;
    p->params.filEgDecayTime = f.filEgDecayTime;
    p->params.filEgDecayShape = f.filEgDecayShape;
    p->params.filEgSustainLevel = f.filEgSustainLevel;
    p->params.filEgReleaseTime = f.filEgReleaseTime;
    p->params.filEgReleaseShape = f.filEgReleaseShape;
    p->params.filterLfoDelay = f.filterLfoDelay;
    p->params.filterLfoFade = f.filterLfoFade;
    p->params.filterLfoDepth = f.filterLfoDepth;
    p->params.filterLfoFreq = f.filterLfoFreq;
    p->params.filterLfoWave = f.filterLfoWave;
    p->params.pitchKeytrack = f.pitchKeytrack;
    p->params.pitchVeltrack = f.pitchVeltrack;
    p->params.pitchRandom = f.pitchRandom;
    p->params.portamentoEnabled = f.portamentoEnabled;
    p->params.glideTime = f.glideTime;
    p->params.pitchEgEnabled = f.pitchEgEnabled;
    p->params.pitchDepth = f.pitchDepth;
    p->params.pitchEgStartLevel = f.pitchEgStartLevel;
    p->params.pitchEgDelayTime = f.pitchEgDelayTime;
    p->params.pitchEgAttackTime = f.pitchEgAttackTime;
    p->params.pitchEgAttackShape = f.pitchEgAttackShape;
    p->params.pitchEgHoldTime = f.pitchEgHoldTime;
    p->params.pitchEgDecayTime = f.pitchEgDecayTime;
    p->params.pitchEgDecayShape = f.pitchEgDecayShape;
    p->params.pitchEgSustainLevel = f.pitchEgSustainLevel;
    p->params.pitchEgReleaseTime = f.pitchEgReleaseTime;
    p->params.pitchEgReleaseShape = f.pitchEgReleaseShape;
    p->params.pitchLfoDelay = f.pitchLfoDelay;
    p->params.pitchLfoFade = f.pitchLfoFade;
    p->params.pitchLfoPitch = f.pitchLfoPitch;
    p->params.pitchLfoFreq = f.pitchLfoFreq;
    p->params.pitchLfoWave = f.pitchLfoWave;
    p->params.ccVolume = f.ccVolume;
    p->params.ccPan = f.ccPan;
    p->params.fxEnabled = f.fxEnabled;
    p->params.fxMode = f.fxMode;
    p->params.fxDetune = f.fxDetune;
    p->params.fxDetuneCcMode = f.fxDetuneCcMode;
    p->params.fxDelay = f.fxDelay;
    p->params.fxDelayCcMode = f.fxDelayCcMode;
    p->params.fxStereoWidth = f.fxStereoWidth;
    p->params.fxDepth = f.fxDepth;
    p->params.fxSpeed = f.fxSpeed;
    p->params.fxWave = f.fxWave;
    p->params.fxPhase = f.fxPhase;
    p->params.fxPhaseCcMode = f.fxPhaseCcMode;
    p->params.fxIndependentLfo = f.fxIndependentLfo;
    p->params.fil2TypeIndex = f.fil2TypeIndex;
    p->params.cutoff2 = f.cutoff2;
    p->params.fxVolume = f.fxVolume;
    p->params.reverbEnabled = f.reverbEnabled;
    p->params.reverbTypeIndex = f.reverbTypeIndex;
    p->params.reverbInput = f.reverbInput;
    p->params.reverbPredelay = f.reverbPredelay;
    p->params.reverbSize = f.reverbSize;
    p->params.reverbTone = f.reverbTone;
    p->params.reverbDamp = f.reverbDamp;
    p->params.reverbDry = f.reverbDry;
    p->params.reverbWet = f.reverbWet;
    p->params.mpeEnabled = f.mpeEnabled;
    p->params.character = f.character;
    p->params.multisampleRootNote = f.multisampleRootNote;
    p->params.panX2 = f.panX2;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.customOpcodesText = f.customOpcodesText;
    }

    if (!applySampleData) {
        regenerateAndLoadSfz(p);
        return;
    }

    restoreStack(p, f.stack);
    regenerateAndLoadSfz(p);
}

bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    Plugin* p = self(plugin);
    std::string customOpcodesText;
    std::vector<SharedParams::GuiState::StackItem> stack;
    std::string scalaFilePath;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        customOpcodesText = p->params.guiState.customOpcodesText;
        stack = p->params.guiState.stack;
        scalaFilePath = p->params.guiState.scalaFilePath;
    }

    bool ok = writeVal(stream, kStateMagic) && writeVal(stream, kStateVersion) &&
             writeVal(stream, p->params.rootNote.load()) &&
             writeVal(stream, p->params.volume.load()) &&
             writeVal(stream, p->params.bendUpCents.load()) &&
             writeVal(stream, p->params.bendDownCents.load()) &&
             writeVal(stream, p->params.quality.load()) &&
             writeVal(stream, p->params.loopModeIndex.load()) &&
             writeVal(stream, p->params.polyphony.load()) &&
             writeVal(stream, p->params.notePolyphony.load()) &&
             writeVal(stream, p->params.tuneCents.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.offsetEnabled.load() ? 1 : 0)) &&
             writeVal(stream, p->params.offsetValue.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.randomOffsetEnabled.load() ? 1 : 0)) &&
             writeVal(stream, p->params.panRandom.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.panAlternate.load() ? 1 : 0)) &&
             writeVal(stream, p->params.panLfoDelay.load()) &&
             writeVal(stream, p->params.panLfoFade.load()) &&
             writeVal(stream, p->params.panLfoPan.load()) &&
             writeVal(stream, p->params.panLfoFreq.load()) &&
             writeVal(stream, p->params.panLfoWave.load()) &&
             writeVal(stream, p->params.ampKeycenter.load()) &&
             writeVal(stream, p->params.ampKeytrack.load()) &&
             writeVal(stream, p->params.ampVeltrack.load()) &&
             writeVal(stream, p->params.ampRandom.load()) &&
             writeVal(stream, p->params.ampStartLevel.load()) &&
             writeVal(stream, p->params.ampDelayTime.load()) &&
             writeVal(stream, p->params.ampAttackTime.load()) &&
             writeVal(stream, p->params.ampAttackShape.load()) &&
             writeVal(stream, p->params.ampHoldTime.load()) &&
             writeVal(stream, p->params.ampDecayTime.load()) &&
             writeVal(stream, p->params.ampDecayShape.load()) &&
             writeVal(stream, p->params.ampSustainLevel.load()) &&
             writeVal(stream, p->params.ampReleaseTime.load()) &&
             writeVal(stream, p->params.ampReleaseShape.load()) &&
             writeVal(stream, p->params.ampLfoDelay.load()) &&
             writeVal(stream, p->params.ampLfoFade.load()) &&
             writeVal(stream, p->params.ampLfoVolume.load()) &&
             writeVal(stream, p->params.ampLfoFreq.load()) &&
             writeVal(stream, p->params.ampLfoWave.load()) &&
             writeVal(stream, p->params.filterTypeIndex.load()) &&
             writeVal(stream, p->params.filterCutoff.load()) &&
             writeVal(stream, p->params.filterResonance.load()) &&
             writeVal(stream, p->params.filterRandomCutoff.load()) &&
             writeVal(stream, p->params.filKeycenter.load()) &&
             writeVal(stream, p->params.filKeytrack.load()) &&
             writeVal(stream, p->params.filVeltrack.load()) &&
             writeVal(stream, p->params.resoVeltrack.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.filterEgEnabled.load() ? 1 : 0)) &&
             writeVal(stream, p->params.filDepth.load()) &&
             writeVal(stream, p->params.filEgStartLevel.load()) &&
             writeVal(stream, p->params.filEgDelayTime.load()) &&
             writeVal(stream, p->params.filEgAttackTime.load()) &&
             writeVal(stream, p->params.filEgAttackShape.load()) &&
             writeVal(stream, p->params.filEgHoldTime.load()) &&
             writeVal(stream, p->params.filEgDecayTime.load()) &&
             writeVal(stream, p->params.filEgDecayShape.load()) &&
             writeVal(stream, p->params.filEgSustainLevel.load()) &&
             writeVal(stream, p->params.filEgReleaseTime.load()) &&
             writeVal(stream, p->params.filEgReleaseShape.load()) &&
             writeVal(stream, p->params.filterLfoDelay.load()) &&
             writeVal(stream, p->params.filterLfoFade.load()) &&
             writeVal(stream, p->params.filterLfoDepth.load()) &&
             writeVal(stream, p->params.filterLfoFreq.load()) &&
             writeVal(stream, p->params.filterLfoWave.load()) &&
             writeVal(stream, p->params.pitchKeytrack.load()) &&
             writeVal(stream, p->params.pitchVeltrack.load()) &&
             writeVal(stream, p->params.pitchRandom.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.portamentoEnabled.load() ? 1 : 0)) &&
             writeVal(stream, p->params.glideTime.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.pitchEgEnabled.load() ? 1 : 0)) &&
             writeVal(stream, p->params.pitchDepth.load()) &&
             writeVal(stream, p->params.pitchEgStartLevel.load()) &&
             writeVal(stream, p->params.pitchEgDelayTime.load()) &&
             writeVal(stream, p->params.pitchEgAttackTime.load()) &&
             writeVal(stream, p->params.pitchEgAttackShape.load()) &&
             writeVal(stream, p->params.pitchEgHoldTime.load()) &&
             writeVal(stream, p->params.pitchEgDecayTime.load()) &&
             writeVal(stream, p->params.pitchEgDecayShape.load()) &&
             writeVal(stream, p->params.pitchEgSustainLevel.load()) &&
             writeVal(stream, p->params.pitchEgReleaseTime.load()) &&
             writeVal(stream, p->params.pitchEgReleaseShape.load()) &&
             writeVal(stream, p->params.pitchLfoDelay.load()) &&
             writeVal(stream, p->params.pitchLfoFade.load()) &&
             writeVal(stream, p->params.pitchLfoPitch.load()) &&
             writeVal(stream, p->params.pitchLfoFreq.load()) &&
             writeVal(stream, p->params.pitchLfoWave.load()) &&
             writeVal(stream, p->params.ccVolume.load()) &&
             writeVal(stream, p->params.ccPan.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.fxEnabled.load() ? 1 : 0)) &&
             writeVal(stream, p->params.fxMode.load()) &&
             writeVal(stream, p->params.fxDetune.load()) &&
             writeVal(stream, p->params.fxDetuneCcMode.load()) &&
             writeVal(stream, p->params.fxDelay.load()) &&
             writeVal(stream, p->params.fxDelayCcMode.load()) &&
             writeVal(stream, p->params.fxStereoWidth.load()) &&
             writeVal(stream, p->params.fxDepth.load()) &&
             writeVal(stream, p->params.fxSpeed.load()) &&
             writeVal(stream, p->params.fxWave.load()) &&
             writeVal(stream, p->params.fxPhase.load()) &&
             writeVal(stream, p->params.fxPhaseCcMode.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.fxIndependentLfo.load() ? 1 : 0)) &&
             writeVal(stream, p->params.fil2TypeIndex.load()) &&
             writeVal(stream, p->params.cutoff2.load()) &&
             writeVal(stream, p->params.fxVolume.load()) &&
             writeVal(stream, static_cast<uint8_t>(p->params.reverbEnabled.load() ? 1 : 0)) &&
             writeVal(stream, p->params.reverbTypeIndex.load()) &&
             writeVal(stream, p->params.reverbInput.load()) &&
             writeVal(stream, p->params.reverbPredelay.load()) &&
             writeVal(stream, p->params.reverbSize.load()) &&
             writeVal(stream, p->params.reverbTone.load()) &&
             writeVal(stream, p->params.reverbDamp.load()) &&
             writeVal(stream, p->params.reverbDry.load()) &&
             writeVal(stream, p->params.reverbWet.load());
    ok = ok && writeVal(stream, static_cast<uint32_t>(customOpcodesText.size()));
    if (ok && !customOpcodesText.empty())
        ok = streamWriteAll(stream, customOpcodesText.data(), customOpcodesText.size());
    ok = ok && writeVal(stream, static_cast<uint32_t>(stack.size()));
    for (const auto& item : stack) {
        if (!ok) break;
        ok = writeVal(stream, static_cast<uint32_t>(item.path.size()));
        if (ok && !item.path.empty())
            ok = streamWriteAll(stream, item.path.data(), item.path.size());
        ok = ok && writeVal(stream, static_cast<uint8_t>(item.isSfz ? 1 : 0));
        ok = ok && writeVal(stream, static_cast<uint32_t>(item.regionsText.size()));
        if (ok && !item.regionsText.empty())
            ok = streamWriteAll(stream, item.regionsText.data(), item.regionsText.size());
    }
    ok = ok && writeVal(stream, static_cast<uint8_t>(p->params.mpeEnabled.load() ? 1 : 0));
    ok = ok && writeVal(stream, p->params.character.load());
    ok = ok && writeVal(stream, p->params.multisampleRootNote.load());
    ok = ok && writeVal(stream, static_cast<uint8_t>(p->params.panX2.load() ? 1 : 0));
    // v22: Pitch tab's Tuning section - DAW-session-only (see
    // SharedParams::tuningFrequency's own comment), so these live directly
    // in the CLAP state stream rather than going through PresetFields like
    // everything above.
    ok = ok && writeVal(stream, p->params.tuningFrequency.load());
    ok = ok && writeVal(stream, static_cast<uint32_t>(scalaFilePath.size()));
    if (ok && !scalaFilePath.empty())
        ok = streamWriteAll(stream, scalaFilePath.data(), scalaFilePath.size());
    return ok;
}

bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    Plugin* p = self(plugin);

    uint32_t magic = 0, version = 0;
    if (!readVal(stream, magic) || magic != kStateMagic) return false;
    if (!readVal(stream, version) || version != kStateVersion) return false;

    int32_t rootNote = 60, volume = 0, bendUp = 2400, bendDown = -2400, quality = 2,
           loopModeIndex = 4, polyphony = 256, notePolyphony = 256, offsetValue = 0,
           ccVolume = 100, ccPan = 64, ampLfoWave = 1, ampKeycenter = 60, ampKeytrack = 0,
           ampVeltrack = 95, ampRandom = 0, panRandom = 0, panLfoPan = 0, panLfoWave = 1;
    float tuneCents = 0.f;
    float panLfoDelay = 0.0f, panLfoFade = 0.0f, panLfoFreq = 10.0f;
    uint8_t panAlternate = 0;
    float ampStartLevel = 0.0f, ampDelayTime = 0.00001f, ampAttackTime = 0.00001f,
         ampAttackShape = 0.00001f, ampHoldTime = 0.00001f, ampDecayTime = 0.00001f,
         ampDecayShape = -0.3616f, ampSustainLevel = 1.0f, ampReleaseTime = 0.00001f,
         ampReleaseShape = -6.3616f, ampLfoDelay = 0.0f, ampLfoFade = 0.0f,
         ampLfoVolume = 0.0f, ampLfoFreq = 10.0f;
    uint8_t offsetEnabled = 0, randomOffsetEnabled = 0;
    int32_t filterTypeIndex = 1, filterCutoff = 20000, filterRandomCutoff = 0,
           filKeycenter = 60, filKeytrack = 0, filVeltrack = 0, resoVeltrack = 0,
           filDepth = 0, filterLfoDepth = 0, filterLfoWave = 1;
    float filterResonance = 0.0f;
    float filEgStartLevel = 0.0f, filEgDelayTime = 0.00001f, filEgAttackTime = 0.00001f,
         filEgAttackShape = 0.00001f, filEgHoldTime = 0.00001f, filEgDecayTime = 0.00001f,
         filEgDecayShape = 0.00001f, filEgSustainLevel = 1.0f, filEgReleaseTime = 0.00001f,
         filEgReleaseShape = 0.00001f, filterLfoDelay = 0.0f, filterLfoFade = 0.0f,
         filterLfoFreq = 10.0f;
    uint8_t filterEgEnabled = 0;
    int32_t pitchKeytrack = 100, pitchVeltrack = 0, pitchRandom = 0, pitchDepth = 0,
           pitchLfoPitch = 0, pitchLfoWave = 1;
    float pitchEgStartLevel = 0.0f, pitchEgDelayTime = 0.00001f, pitchEgAttackTime = 0.00001f,
         pitchEgAttackShape = 0.00001f, pitchEgHoldTime = 0.00001f, pitchEgDecayTime = 0.00001f,
         pitchEgDecayShape = 0.00001f, pitchEgSustainLevel = 1.0f, pitchEgReleaseTime = 0.00001f,
         pitchEgReleaseShape = 0.00001f, pitchLfoDelay = 0.0f, pitchLfoFade = 0.0f,
         pitchLfoFreq = 4.5f;
    uint8_t pitchEgEnabled = 0;
    float glideTime = 0.0f;
    uint8_t portamentoEnabled = 0;
    uint8_t fxEnabled = 0;
    int32_t fxMode = 0, fxDetune = 0, fxDetuneCcMode = 0, fxDelayCcMode = 0,
           fxStereoWidth = 0, fxDepth = 0, fxWave = 1, fxPhaseCcMode = 0;
    float fxDelay = 0.0f, fxSpeed = 5.0f, fxPhase = 0.0f;
    uint8_t fxIndependentLfo = 0;
    int32_t fil2TypeIndex = 1, cutoff2 = 11700, fxVolume = -6;
    uint8_t reverbEnabled = 0;
    int32_t reverbTypeIndex = 0;
    float reverbInput = 100.0f, reverbPredelay = 50.0f, reverbSize = 50.0f,
         reverbTone = 50.0f, reverbDamp = 50.0f, reverbDry = 100.0f, reverbWet = 100.0f;
    if (!readVal(stream, rootNote) || !readVal(stream, volume) || !readVal(stream, bendUp) ||
        !readVal(stream, bendDown) || !readVal(stream, quality) ||
        !readVal(stream, loopModeIndex) || !readVal(stream, polyphony) ||
        !readVal(stream, notePolyphony) || !readVal(stream, tuneCents) ||
        !readVal(stream, offsetEnabled) || !readVal(stream, offsetValue) ||
        !readVal(stream, randomOffsetEnabled) ||
        !readVal(stream, panRandom) || !readVal(stream, panAlternate) ||
        !readVal(stream, panLfoDelay) || !readVal(stream, panLfoFade) ||
        !readVal(stream, panLfoPan) || !readVal(stream, panLfoFreq) ||
        !readVal(stream, panLfoWave) ||
        !readVal(stream, ampKeycenter) || !readVal(stream, ampKeytrack) ||
        !readVal(stream, ampVeltrack) || !readVal(stream, ampRandom) ||
        !readVal(stream, ampStartLevel) || !readVal(stream, ampDelayTime) ||
        !readVal(stream, ampAttackTime) || !readVal(stream, ampAttackShape) ||
        !readVal(stream, ampHoldTime) || !readVal(stream, ampDecayTime) ||
        !readVal(stream, ampDecayShape) || !readVal(stream, ampSustainLevel) ||
        !readVal(stream, ampReleaseTime) || !readVal(stream, ampReleaseShape) ||
        !readVal(stream, ampLfoDelay) || !readVal(stream, ampLfoFade) ||
        !readVal(stream, ampLfoVolume) || !readVal(stream, ampLfoFreq) ||
        !readVal(stream, ampLfoWave) ||
        !readVal(stream, filterTypeIndex) || !readVal(stream, filterCutoff) ||
        !readVal(stream, filterResonance) || !readVal(stream, filterRandomCutoff) ||
        !readVal(stream, filKeycenter) || !readVal(stream, filKeytrack) ||
        !readVal(stream, filVeltrack) || !readVal(stream, resoVeltrack) ||
        !readVal(stream, filterEgEnabled) || !readVal(stream, filDepth) ||
        !readVal(stream, filEgStartLevel) || !readVal(stream, filEgDelayTime) ||
        !readVal(stream, filEgAttackTime) || !readVal(stream, filEgAttackShape) ||
        !readVal(stream, filEgHoldTime) || !readVal(stream, filEgDecayTime) ||
        !readVal(stream, filEgDecayShape) || !readVal(stream, filEgSustainLevel) ||
        !readVal(stream, filEgReleaseTime) || !readVal(stream, filEgReleaseShape) ||
        !readVal(stream, filterLfoDelay) || !readVal(stream, filterLfoFade) ||
        !readVal(stream, filterLfoDepth) || !readVal(stream, filterLfoFreq) ||
        !readVal(stream, filterLfoWave) ||
        !readVal(stream, pitchKeytrack) || !readVal(stream, pitchVeltrack) ||
        !readVal(stream, pitchRandom) ||
        !readVal(stream, portamentoEnabled) || !readVal(stream, glideTime) ||
        !readVal(stream, pitchEgEnabled) ||
        !readVal(stream, pitchDepth) ||
        !readVal(stream, pitchEgStartLevel) || !readVal(stream, pitchEgDelayTime) ||
        !readVal(stream, pitchEgAttackTime) || !readVal(stream, pitchEgAttackShape) ||
        !readVal(stream, pitchEgHoldTime) || !readVal(stream, pitchEgDecayTime) ||
        !readVal(stream, pitchEgDecayShape) || !readVal(stream, pitchEgSustainLevel) ||
        !readVal(stream, pitchEgReleaseTime) || !readVal(stream, pitchEgReleaseShape) ||
        !readVal(stream, pitchLfoDelay) || !readVal(stream, pitchLfoFade) ||
        !readVal(stream, pitchLfoPitch) || !readVal(stream, pitchLfoFreq) ||
        !readVal(stream, pitchLfoWave) ||
        !readVal(stream, ccVolume) || !readVal(stream, ccPan) ||
        !readVal(stream, fxEnabled) || !readVal(stream, fxMode) ||
        !readVal(stream, fxDetune) || !readVal(stream, fxDetuneCcMode) ||
        !readVal(stream, fxDelay) || !readVal(stream, fxDelayCcMode) ||
        !readVal(stream, fxStereoWidth) || !readVal(stream, fxDepth) ||
        !readVal(stream, fxSpeed) || !readVal(stream, fxWave) ||
        !readVal(stream, fxPhase) || !readVal(stream, fxPhaseCcMode) ||
        !readVal(stream, fxIndependentLfo) ||
        !readVal(stream, fil2TypeIndex) || !readVal(stream, cutoff2) ||
        !readVal(stream, fxVolume) ||
        !readVal(stream, reverbEnabled) ||
        !readVal(stream, reverbTypeIndex) || !readVal(stream, reverbInput) ||
        !readVal(stream, reverbPredelay) || !readVal(stream, reverbSize) ||
        !readVal(stream, reverbTone) || !readVal(stream, reverbDamp) ||
        !readVal(stream, reverbDry) || !readVal(stream, reverbWet))
        return false;

    uint32_t customOpcodesLen = 0;
    if (!readVal(stream, customOpcodesLen)) return false;
    std::string customOpcodesText;
    if (customOpcodesLen > 0) {
        customOpcodesText.resize(customOpcodesLen);
        if (!streamReadAll(stream, customOpcodesText.data(), customOpcodesLen)) return false;
    }

    uint32_t stackCount = 0;
    if (!readVal(stream, stackCount)) return false;
    std::vector<PresetStackItem> stack;
    stack.reserve(stackCount);
    for (uint32_t i = 0; i < stackCount; ++i) {
        uint32_t pathLen = 0;
        if (!readVal(stream, pathLen)) return false;
        PresetStackItem item;
        if (pathLen > 0) {
            item.path.resize(pathLen);
            if (!streamReadAll(stream, item.path.data(), pathLen)) return false;
        }
        uint8_t isSfz = 0;
        if (!readVal(stream, isSfz)) return false;
        item.isSfz = isSfz != 0;
        uint32_t regionsLen = 0;
        if (!readVal(stream, regionsLen)) return false;
        if (regionsLen > 0) {
            item.regionsText.resize(regionsLen);
            if (!streamReadAll(stream, item.regionsText.data(), regionsLen)) return false;
        }
        stack.push_back(std::move(item));
    }

    uint8_t mpeEnabled = 0;
    if (!readVal(stream, mpeEnabled)) return false;
    int32_t character = 0;
    if (!readVal(stream, character)) return false;
    int32_t multisampleRootNote = 60;
    if (!readVal(stream, multisampleRootNote)) return false;
    uint8_t panX2 = 0;
    if (!readVal(stream, panX2)) return false;

    // v22: Pitch tab's Tuning section - DAW-session-only, read directly
    // rather than through PresetFields (see stateSave's matching write).
    float tuningFrequency = 440.0f;
    if (!readVal(stream, tuningFrequency)) return false;
    uint32_t scalaPathLen = 0;
    if (!readVal(stream, scalaPathLen)) return false;
    std::string scalaFilePath;
    if (scalaPathLen > 0) {
        scalaFilePath.resize(scalaPathLen);
        if (!streamReadAll(stream, scalaFilePath.data(), scalaPathLen)) return false;
    }

    PresetFields f;
    f.rootNote = rootNote;
    f.volume = volume;
    f.bendUpCents = bendUp;
    f.bendDownCents = bendDown;
    f.quality = quality;
    f.loopModeIndex = loopModeIndex;
    f.polyphony = polyphony;
    f.notePolyphony = notePolyphony;
    f.tuneCents = tuneCents;
    f.offsetEnabled = offsetEnabled != 0;
    f.offsetValue = offsetValue;
    f.randomOffsetEnabled = randomOffsetEnabled != 0;
    f.panRandom = panRandom;
    f.panAlternate = panAlternate != 0;
    f.panLfoDelay = panLfoDelay;
    f.panLfoFade = panLfoFade;
    f.panLfoPan = panLfoPan;
    f.panLfoFreq = panLfoFreq;
    f.panLfoWave = panLfoWave;
    f.ampKeycenter = ampKeycenter;
    f.ampKeytrack = ampKeytrack;
    f.ampVeltrack = ampVeltrack;
    f.ampRandom = ampRandom;
    f.ampStartLevel = ampStartLevel;
    f.ampDelayTime = ampDelayTime;
    f.ampAttackTime = ampAttackTime;
    f.ampAttackShape = ampAttackShape;
    f.ampHoldTime = ampHoldTime;
    f.ampDecayTime = ampDecayTime;
    f.ampDecayShape = ampDecayShape;
    f.ampSustainLevel = ampSustainLevel;
    f.ampReleaseTime = ampReleaseTime;
    f.ampReleaseShape = ampReleaseShape;
    f.ampLfoDelay = ampLfoDelay;
    f.ampLfoFade = ampLfoFade;
    f.ampLfoVolume = ampLfoVolume;
    f.ampLfoFreq = ampLfoFreq;
    f.ampLfoWave = ampLfoWave;
    f.filterTypeIndex = filterTypeIndex;
    f.filterCutoff = filterCutoff;
    f.filterResonance = filterResonance;
    f.filterRandomCutoff = filterRandomCutoff;
    f.filKeycenter = filKeycenter;
    f.filKeytrack = filKeytrack;
    f.filVeltrack = filVeltrack;
    f.resoVeltrack = resoVeltrack;
    f.filterEgEnabled = filterEgEnabled != 0;
    f.filDepth = filDepth;
    f.filEgStartLevel = filEgStartLevel;
    f.filEgDelayTime = filEgDelayTime;
    f.filEgAttackTime = filEgAttackTime;
    f.filEgAttackShape = filEgAttackShape;
    f.filEgHoldTime = filEgHoldTime;
    f.filEgDecayTime = filEgDecayTime;
    f.filEgDecayShape = filEgDecayShape;
    f.filEgSustainLevel = filEgSustainLevel;
    f.filEgReleaseTime = filEgReleaseTime;
    f.filEgReleaseShape = filEgReleaseShape;
    f.filterLfoDelay = filterLfoDelay;
    f.filterLfoFade = filterLfoFade;
    f.filterLfoDepth = filterLfoDepth;
    f.filterLfoFreq = filterLfoFreq;
    f.filterLfoWave = filterLfoWave;
    f.pitchKeytrack = pitchKeytrack;
    f.pitchVeltrack = pitchVeltrack;
    f.pitchRandom = pitchRandom;
    f.portamentoEnabled = portamentoEnabled != 0;
    f.glideTime = glideTime;
    f.pitchEgEnabled = pitchEgEnabled != 0;
    f.pitchDepth = pitchDepth;
    f.pitchEgStartLevel = pitchEgStartLevel;
    f.pitchEgDelayTime = pitchEgDelayTime;
    f.pitchEgAttackTime = pitchEgAttackTime;
    f.pitchEgAttackShape = pitchEgAttackShape;
    f.pitchEgHoldTime = pitchEgHoldTime;
    f.pitchEgDecayTime = pitchEgDecayTime;
    f.pitchEgDecayShape = pitchEgDecayShape;
    f.pitchEgSustainLevel = pitchEgSustainLevel;
    f.pitchEgReleaseTime = pitchEgReleaseTime;
    f.pitchEgReleaseShape = pitchEgReleaseShape;
    f.pitchLfoDelay = pitchLfoDelay;
    f.pitchLfoFade = pitchLfoFade;
    f.pitchLfoPitch = pitchLfoPitch;
    f.pitchLfoFreq = pitchLfoFreq;
    f.pitchLfoWave = pitchLfoWave;
    f.ccVolume = ccVolume;
    f.ccPan = ccPan;
    f.fxEnabled = fxEnabled != 0;
    f.fxMode = fxMode;
    f.fxDetune = fxDetune;
    f.fxDetuneCcMode = fxDetuneCcMode;
    f.fxDelay = fxDelay;
    f.fxDelayCcMode = fxDelayCcMode;
    f.fxStereoWidth = fxStereoWidth;
    f.fxDepth = fxDepth;
    f.fxSpeed = fxSpeed;
    f.fxWave = fxWave;
    f.fxPhase = fxPhase;
    f.fxPhaseCcMode = fxPhaseCcMode;
    f.fxIndependentLfo = fxIndependentLfo != 0;
    f.fil2TypeIndex = fil2TypeIndex;
    f.cutoff2 = cutoff2;
    f.fxVolume = fxVolume;
    f.reverbEnabled = reverbEnabled != 0;
    f.reverbTypeIndex = reverbTypeIndex;
    f.reverbInput = reverbInput;
    f.reverbPredelay = reverbPredelay;
    f.reverbSize = reverbSize;
    f.reverbTone = reverbTone;
    f.reverbDamp = reverbDamp;
    f.reverbDry = reverbDry;
    f.reverbWet = reverbWet;
    f.customOpcodesText = customOpcodesText;
    f.mpeEnabled = mpeEnabled != 0;
    f.character = character;
    f.multisampleRootNote = multisampleRootNote;
    f.panX2 = panX2 != 0;
    f.stack = std::move(stack);

    applyPresetFields(p, f, /*applySampleData=*/true);

    // v22: Tuning is DAW-session-only, restored directly rather than through
    // applyPresetFields (which is shared with Preset/Profile loads that must
    // NOT touch it - see SharedParams::tuningFrequency's own comment).
    p->params.tuningFrequency = tuningFrequency;
    restoreScala(p, scalaFilePath);

    return true;
}

const clap_plugin_state_t kExtState = {
    .save = stateSave,
    .load = stateLoad,
};

// ---------------------------------------------------------- preset/profile
//
// Local-only .sspreset (everything, including the sample/multisample-SFZ
// reference) / .ssprofile (everything except that reference - see
// PresetFile.h) save-load, picked via a native OS dialog (zenity, see
// drawEditorUI's preset button row) rather than the in-house ImGui
// FileDialog the sample/sfz loader uses - deliberately a different format
// and file from the CLAP host state above (kStateMagic/kStateVersion):
// these are user-managed files, not a DAW project's own save data.

std::string presetsDir() {
    const char* home = getenv("HOME");
    return (home ? std::string(home) : std::string(".")) + "/SoloSamplerPresets";
}

// Derives a default filename from the stack's first item (basename,
// extension stripped), else "Untitled" - GUI thread only.
std::string defaultPresetBaseName(Plugin* p) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        if (!p->params.guiState.stack.empty()) path = p->params.guiState.stack.front().path;
    }
    if (path.empty()) return "Untitled";
    std::filesystem::path fsPath(path);
    return fsPath.stem().string();
}

// Snapshots every instrument-design field in `params` into a PresetFields
// (everything gatherPresetFields normally reads from the live plugin,
// EXCEPT the sample/SFZ stack, which the two callers below handle
// differently - left default-empty here). Factored out so a fresh,
// throwaway SharedParams{} (the single authoritative place every field's
// true default lives) can be passed to it just as validly as a running
// instance's real p->params - see handleResetToDefault, which relies on
// this to avoid a second, hand-copied set of default literals that could
// drift out of sync with shared.hpp (exactly what happened to
// PresetFile.h's own fxDelay/fxDelayCcMode/fxDepth/fxDetune/fxPhase/
// fxPhaseCcMode/fxSpeed/fxWave defaults before it was noticed and fixed).
PresetFields fieldsFromParams(SharedParams& params) {
    PresetFields f;
    f.rootNote = params.rootNote.load();
    f.volume = params.volume.load();
    f.bendUpCents = params.bendUpCents.load();
    f.bendDownCents = params.bendDownCents.load();
    f.quality = params.quality.load();
    f.loopModeIndex = params.loopModeIndex.load();
    f.polyphony = params.polyphony.load();
    f.notePolyphony = params.notePolyphony.load();
    f.tuneCents = params.tuneCents.load();
    f.offsetEnabled = params.offsetEnabled.load();
    f.offsetValue = params.offsetValue.load();
    f.randomOffsetEnabled = params.randomOffsetEnabled.load();
    f.panRandom = params.panRandom.load();
    f.panAlternate = params.panAlternate.load();
    f.panLfoDelay = params.panLfoDelay.load();
    f.panLfoFade = params.panLfoFade.load();
    f.panLfoPan = params.panLfoPan.load();
    f.panLfoFreq = params.panLfoFreq.load();
    f.panLfoWave = params.panLfoWave.load();
    f.ampKeycenter = params.ampKeycenter.load();
    f.ampKeytrack = params.ampKeytrack.load();
    f.ampVeltrack = params.ampVeltrack.load();
    f.ampRandom = params.ampRandom.load();
    f.ampStartLevel = params.ampStartLevel.load();
    f.ampDelayTime = params.ampDelayTime.load();
    f.ampAttackTime = params.ampAttackTime.load();
    f.ampAttackShape = params.ampAttackShape.load();
    f.ampHoldTime = params.ampHoldTime.load();
    f.ampDecayTime = params.ampDecayTime.load();
    f.ampDecayShape = params.ampDecayShape.load();
    f.ampSustainLevel = params.ampSustainLevel.load();
    f.ampReleaseTime = params.ampReleaseTime.load();
    f.ampReleaseShape = params.ampReleaseShape.load();
    f.ampLfoDelay = params.ampLfoDelay.load();
    f.ampLfoFade = params.ampLfoFade.load();
    f.ampLfoVolume = params.ampLfoVolume.load();
    f.ampLfoFreq = params.ampLfoFreq.load();
    f.ampLfoWave = params.ampLfoWave.load();
    f.filterTypeIndex = params.filterTypeIndex.load();
    f.filterCutoff = params.filterCutoff.load();
    f.filterResonance = params.filterResonance.load();
    f.filterRandomCutoff = params.filterRandomCutoff.load();
    f.filKeycenter = params.filKeycenter.load();
    f.filKeytrack = params.filKeytrack.load();
    f.filVeltrack = params.filVeltrack.load();
    f.resoVeltrack = params.resoVeltrack.load();
    f.filterEgEnabled = params.filterEgEnabled.load();
    f.filDepth = params.filDepth.load();
    f.filEgStartLevel = params.filEgStartLevel.load();
    f.filEgDelayTime = params.filEgDelayTime.load();
    f.filEgAttackTime = params.filEgAttackTime.load();
    f.filEgAttackShape = params.filEgAttackShape.load();
    f.filEgHoldTime = params.filEgHoldTime.load();
    f.filEgDecayTime = params.filEgDecayTime.load();
    f.filEgDecayShape = params.filEgDecayShape.load();
    f.filEgSustainLevel = params.filEgSustainLevel.load();
    f.filEgReleaseTime = params.filEgReleaseTime.load();
    f.filEgReleaseShape = params.filEgReleaseShape.load();
    f.filterLfoDelay = params.filterLfoDelay.load();
    f.filterLfoFade = params.filterLfoFade.load();
    f.filterLfoDepth = params.filterLfoDepth.load();
    f.filterLfoFreq = params.filterLfoFreq.load();
    f.filterLfoWave = params.filterLfoWave.load();
    f.pitchKeytrack = params.pitchKeytrack.load();
    f.pitchVeltrack = params.pitchVeltrack.load();
    f.pitchRandom = params.pitchRandom.load();
    f.portamentoEnabled = params.portamentoEnabled.load();
    f.glideTime = params.glideTime.load();
    f.pitchEgEnabled = params.pitchEgEnabled.load();
    f.pitchDepth = params.pitchDepth.load();
    f.pitchEgStartLevel = params.pitchEgStartLevel.load();
    f.pitchEgDelayTime = params.pitchEgDelayTime.load();
    f.pitchEgAttackTime = params.pitchEgAttackTime.load();
    f.pitchEgAttackShape = params.pitchEgAttackShape.load();
    f.pitchEgHoldTime = params.pitchEgHoldTime.load();
    f.pitchEgDecayTime = params.pitchEgDecayTime.load();
    f.pitchEgDecayShape = params.pitchEgDecayShape.load();
    f.pitchEgSustainLevel = params.pitchEgSustainLevel.load();
    f.pitchEgReleaseTime = params.pitchEgReleaseTime.load();
    f.pitchEgReleaseShape = params.pitchEgReleaseShape.load();
    f.pitchLfoDelay = params.pitchLfoDelay.load();
    f.pitchLfoFade = params.pitchLfoFade.load();
    f.pitchLfoPitch = params.pitchLfoPitch.load();
    f.pitchLfoFreq = params.pitchLfoFreq.load();
    f.pitchLfoWave = params.pitchLfoWave.load();
    f.ccVolume = params.ccVolume.load();
    f.ccPan = params.ccPan.load();
    f.fxEnabled = params.fxEnabled.load();
    f.fxMode = params.fxMode.load();
    f.fxDetune = params.fxDetune.load();
    f.fxDetuneCcMode = params.fxDetuneCcMode.load();
    f.fxDelay = params.fxDelay.load();
    f.fxDelayCcMode = params.fxDelayCcMode.load();
    f.fxStereoWidth = params.fxStereoWidth.load();
    f.fxDepth = params.fxDepth.load();
    f.fxSpeed = params.fxSpeed.load();
    f.fxWave = params.fxWave.load();
    f.fxPhase = params.fxPhase.load();
    f.fxPhaseCcMode = params.fxPhaseCcMode.load();
    f.fxIndependentLfo = params.fxIndependentLfo.load();
    f.fil2TypeIndex = params.fil2TypeIndex.load();
    f.cutoff2 = params.cutoff2.load();
    f.fxVolume = params.fxVolume.load();
    f.reverbEnabled = params.reverbEnabled.load();
    f.reverbTypeIndex = params.reverbTypeIndex.load();
    f.reverbInput = params.reverbInput.load();
    f.reverbPredelay = params.reverbPredelay.load();
    f.reverbSize = params.reverbSize.load();
    f.reverbTone = params.reverbTone.load();
    f.reverbDamp = params.reverbDamp.load();
    f.reverbDry = params.reverbDry.load();
    f.reverbWet = params.reverbWet.load();
    f.mpeEnabled = params.mpeEnabled.load();
    f.character = params.character.load();
    f.multisampleRootNote = params.multisampleRootNote.load();
    f.panX2 = params.panX2.load();
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        f.customOpcodesText = params.guiState.customOpcodesText;
    }
    return f;
}

PresetFields gatherPresetFields(Plugin* p) {
    PresetFields f = fieldsFromParams(p->params);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    f.stack.reserve(p->params.guiState.stack.size());
    for (const auto& item : p->params.guiState.stack)
        f.stack.push_back({item.path, item.isSfz, item.isSfz ? item.regionsText : std::string()});
    return f;
}

bool saveSoloSamplerPreset(Plugin* p, const std::string& path, bool includeSampleData) {
    return writePresetFile(path, includeSampleData ? 0 : 1, gatherPresetFields(p));
}

// "Reset to Default" button handler (persistent row, next to Export SFZ;
// the confirmation prompt itself lives in editor_ui.cpp - this only runs
// once the user has already confirmed). Restores every instrument-design
// parameter to its true declared default, sourced from a fresh, throwaway
// SharedParams instance via fieldsFromParams() rather than a second hand-
// copied set of default literals (see that function's own comment on why).
// Goes through applyPresetFields with applySampleData=false - the exact
// same "everything except sample identity" path Load Profile already uses -
// so the loaded sample/SFZ stack is left completely untouched; Scala tuning
// and the tuning frequency are untouched too, simply because neither is
// part of PresetFields at all (DAW-session-only, see
// SharedParams::tuningFrequency's own comment). GUI thread only.
void handleResetToDefault(Plugin* p) {
    SharedParams defaults;
    applyPresetFields(p, fieldsFromParams(defaults), /*applySampleData=*/false);
}

bool loadSoloSamplerPreset(Plugin* p, const std::string& path, bool applySampleData,
                           std::string& outError) {
    PresetFileResult result = readPresetFile(path);
    if (!result.ok) {
        outError = result.error;
        return false;
    }
    if (applySampleData && result.kind != 0) {
        outError = "This is a Profile file - use Load Profile instead: " + path;
        return false;
    }
    applyPresetFields(p, result.fields, applySampleData);
    return true;
}

// Save Preset/Save Profile button handler: opens zenity's save dialog
// (seeded with ~/SoloSamplerPresets/<current sample or sfz name>.ssXXX),
// appends the right extension if the user didn't type one (same courtesy
// the in-house FileDialog's own SaveFile mode already does - see
// file_dialog.cpp's tryAccept), then saves. Cancelling zenity (empty path)
// is silent, not an error.
void handleSavePresetOrProfile(Plugin* p, bool includeSampleData) {
    std::filesystem::create_directories(presetsDir());
    const std::string ext = includeSampleData ? ".sspreset" : ".ssprofile";
    const std::string defaultPath = presetsDir() + "/" + defaultPresetBaseName(p) + ext;
    const std::string title = includeSampleData ? "Save Preset" : "Save Profile";
    const std::string filterName = includeSampleData ? "SoloSampler Preset" : "SoloSampler Profile";

    std::string path = zenitySaveFile(title, defaultPath, {{filterName, "*" + ext}});
    if (path.empty()) return;
    if (path.size() < ext.size() || path.compare(path.size() - ext.size(), ext.size(), ext) != 0)
        path += ext;

    bool ok = saveSoloSamplerPreset(p, path, includeSampleData);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    p->params.guiState.presetError = ok ? std::string() : ("Could not save: " + path);
}

// Load Preset/Load Profile button handler. Load Profile's filter accepts
// BOTH .ssprofile and .sspreset (loadSoloSamplerPreset only applies the
// non-sample fields either way when applySampleData is false) - Load
// Preset only accepts .sspreset. Cancelling zenity is silent.
void handleLoadPresetOrProfile(Plugin* p, bool applySampleData) {
    std::string path = applySampleData
        ? zenityOpenFile("Load Preset", presetsDir() + "/", {{"SoloSampler Preset", "*.sspreset"}})
        : zenityOpenFile("Load Profile", presetsDir() + "/",
                         {{"SoloSampler Profile/Preset", "*.ssprofile *.sspreset"},
                          {"All files", "*"}});
    if (path.empty()) return;

    std::string err;
    bool ok = loadSoloSamplerPreset(p, path, applySampleData, err);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    p->params.guiState.presetError = ok ? std::string() : err;
}

// Rewrites every sample= opcode value in a generated SFZ text from
// SoloSampler's internal root-relative convention (the sample's real
// absolute path with the leading '/' stripped - see regenerateAndLoadSfz's
// own comment) back to a genuine absolute path. The in-RAM text only
// resolves correctly through sfizz being told its virtual "path" is "/";
// a real SFZ player opening an exported, standalone .sfz file resolves any
// non-absolute sample= relative to THAT file's own directory instead, which
// would silently break every reference unless this restores the leading
// '/' first. sample= is always its own line, at column 0 (see
// buildSfzText/SfzFlatten.cpp), so a simple per-line check is enough - no
// need for a real SFZ tokenizer.
std::string absolutizeSamplePaths(const std::string& sfz) {
    static constexpr const char* kPrefix = "sample=";
    static constexpr size_t kPrefixLen = 7;
    std::string out;
    out.reserve(sfz.size());
    size_t pos = 0;
    while (pos < sfz.size()) {
        size_t lineEnd = sfz.find('\n', pos);
        size_t len = lineEnd == std::string::npos ? sfz.size() - pos : lineEnd - pos;
        if (len > kPrefixLen && sfz.compare(pos, kPrefixLen, kPrefix) == 0 &&
            sfz[pos + kPrefixLen] != '/') {
            out.append(kPrefix);
            out += '/';
            out.append(sfz, pos + kPrefixLen, len - kPrefixLen);
        } else {
            out.append(sfz, pos, len);
        }
        if (lineEnd == std::string::npos) break;
        out += '\n';
        pos = lineEnd + 1;
    }
    return out;
}

// Export SFZ button handler: writes the currently generated SFZ text out as
// a real, standalone .sfz file (sample= paths absolutized first - see
// absolutizeSamplePaths) via zenity's save dialog, same
// seed-folder/extension-append/cancel-is-silent conventions as Save Preset/
// Profile. Sibling to those (shares guiState.presetError for its own
// failure reporting - same button row, same error slot) but otherwise
// unrelated: this is a plain-text SFZ any player can open, not one of
// SoloSampler's own binary formats.
void handleExportSfz(Plugin* p) {
    std::filesystem::create_directories(presetsDir());
    const std::string defaultPath = presetsDir() + "/" + defaultPresetBaseName(p) + ".sfz";

    std::string path = zenitySaveFile("Export SFZ", defaultPath, {{"SFZ Instrument", "*.sfz"}});
    if (path.empty()) return;
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".sfz") != 0) path += ".sfz";

    std::string text;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        text = p->params.guiState.lastSfzText;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    bool ok = static_cast<bool>(out);
    if (ok) {
        out << absolutizeSamplePaths(text);
        ok = static_cast<bool>(out);
    }

    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    p->params.guiState.presetError = ok ? std::string() : ("Could not export: " + path);
}

// -------------------------------------------------------------------- gui

bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool is_floating) {
    return !is_floating && strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* is_floating) {
    *api = CLAP_WINDOW_API_X11;
    *is_floating = false;
    return true;
}

bool guiCreate(const clap_plugin_t* plugin, const char* api, bool is_floating) {
    if (!guiIsApiSupported(plugin, api, is_floating)) return false;
    Plugin* p = self(plugin);
    if (p->window) return true;
    p->window = std::make_unique<GuiWindow>(
        [p] {
            drawEditorUI(
                p->params, p->uiState, [p] { regenerateAndLoadSfz(p); },
                [p](const std::vector<std::string>& paths) { loadStack(p, paths); },
                [p] { handleSavePresetOrProfile(p, true); },
                [p] { handleLoadPresetOrProfile(p, true); },
                [p] { handleSavePresetOrProfile(p, false); },
                [p] { handleLoadPresetOrProfile(p, false); },
                [p] { handleExportSfz(p); },
                [p] { handleResetToDefault(p); },
                [p](size_t index) { removeStackItem(p, index); },
                [p] { clearStack(p); },
                [p] { handleLoadScalaFile(p); },
                [p] { handleClearScala(p); });
        },
        [p](const std::vector<std::string>& paths) { loadStack(p, paths); });
    return p->window->create(kDefaultW, kDefaultH);
}

void guiDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (p->window) {
        p->window->destroy();
        p->window.reset();
    }
}

bool guiSetScale(const clap_plugin_t*, double) { return true; }

bool guiGetSize(const clap_plugin_t* plugin, uint32_t* w, uint32_t* h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->getSize(*w, *h);
    else { *w = kDefaultW; *h = kDefaultH; }
    return true;
}

bool guiCanResize(const clap_plugin_t*) { return true; }

bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    return true;
}

bool guiAdjustSize(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    if (*w < 400) *w = 400;
    if (*h < 480) *h = 480;
    return true;
}

bool guiSetSize(const clap_plugin_t* plugin, uint32_t w, uint32_t h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->setSize(w, h);
    return true;
}

bool guiSetParent(const clap_plugin_t* plugin, const clap_window_t* win) {
    Plugin* p = self(plugin);
    if (!p->window || !win) return false;
    p->window->setParent((unsigned long)win->x11);
    return true;
}

bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return true; }

void guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool guiShow(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(true);
    return true;
}

bool guiHide(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(false);
    return true;
}

const clap_plugin_gui_t kExtGui = {
    .is_api_supported = guiIsApiSupported,
    .get_preferred_api = guiGetPreferredApi,
    .create = guiCreate,
    .destroy = guiDestroy,
    .set_scale = guiSetScale,
    .get_size = guiGetSize,
    .can_resize = guiCanResize,
    .get_resize_hints = guiGetResizeHints,
    .adjust_size = guiAdjustSize,
    .set_size = guiSetSize,
    .set_parent = guiSetParent,
    .set_transient = guiSetTransient,
    .suggest_title = guiSuggestTitle,
    .show = guiShow,
    .hide = guiHide,
};

// ------------------------------------------------------------------ plugin

bool plugInit(const clap_plugin_t*) { return true; }

void plugDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    guiDestroy(plugin);
    delete p;
}

bool plugActivate(const clap_plugin_t* plugin, double sampleRate, uint32_t,
                  uint32_t maxFrames) {
    Plugin* p = self(plugin);
    p->engine.prepare(sampleRate, static_cast<int>(maxFrames));
    // Loads a valid but sample-less <region> - silence is the correct
    // output until a sample is dropped in (phase 4).
    regenerateAndLoadSfz(p);
    return true;
}

void plugDeactivate(const clap_plugin_t*) {}

bool plugStartProcessing(const clap_plugin_t*) { return true; }

void plugStopProcessing(const clap_plugin_t*) {}

void plugReset(const clap_plugin_t*) {}

clap_process_status plugProcess(const clap_plugin_t* plugin, const clap_process_t* process) {
    Plugin* p = self(plugin);
    if (process->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
    const auto& ob = process->audio_outputs[0];
    if (!ob.data32 || !ob.data32[0]) return CLAP_PROCESS_CONTINUE;

    constexpr uint32_t kMaxEvents = 256;
    SfizzEngine::MidiEvent events[kMaxEvents];
    uint32_t numEvents = 0;

    auto logNote = [p](bool isNoteOn, int note, int velocity) {
        uint32_t idx = p->params.midiLogWriteCount.fetch_add(1) % SharedParams::kMidiLogCapacity;
        p->params.midiLog[idx] = {isNoteOn, 1, note, velocity};
    };
    auto noteOnVisual = [p, &logNote](int note, int velocity) {
        if (note < 0 || note > 127) return;
        p->params.noteActive[note] = true;
        p->params.noteVelocity01[note] = velocity / 127.0f;
        logNote(true, note, velocity);
    };
    auto noteOffVisual = [p, &logNote](int note, int velocity) {
        if (note < 0 || note > 127) return;
        p->params.noteActive[note] = false;
        logNote(false, note, velocity);
    };

    // GUI -> audio handoff: preview piano clicks and CC7/CC10, drained once
    // per block, injected ahead of anything parsed from in_events.
    if (numEvents < kMaxEvents && p->params.pendingPreviewNoteOn.exchange(false)) {
        int note = p->params.previewNoteOnNumber.load();
        int vel = p->params.previewNoteOnVelocity.load();
        events[numEvents++] = {SfizzEngine::MidiEvent::Type::NoteOn, 0, note, vel, 0, 0, 0};
        noteOnVisual(note, vel);
    }
    if (numEvents < kMaxEvents && p->params.pendingPreviewNoteOff.exchange(false)) {
        int note = p->params.previewNoteOffNumber.load();
        events[numEvents++] = {SfizzEngine::MidiEvent::Type::NoteOff, 0, note, 0, 0, 0, 0};
        noteOffVisual(note, 0);
    }
    if (numEvents < kMaxEvents && p->params.pendingCcVolume.exchange(false))
        events[numEvents++] = {SfizzEngine::MidiEvent::Type::CC, 0, 0, 0, 7,
                               p->params.ccVolume.load(), 0};
    if (numEvents < kMaxEvents && p->params.pendingCcPan.exchange(false))
        events[numEvents++] = {SfizzEngine::MidiEvent::Type::CC, 0, 0, 0, 10,
                               p->params.ccPan.load(), 0};

    const uint32_t inCount = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < inCount && numEvents < kMaxEvents; ++i) {
        const clap_event_header_t* hdr = process->in_events->get(process->in_events, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID || hdr->type != CLAP_EVENT_MIDI)
            continue;
        const auto* midi = reinterpret_cast<const clap_event_midi_t*>(hdr);
        const int delay = static_cast<int>(hdr->time);
        const uint8_t status = midi->data[0] & 0xF0;
        const int d1 = midi->data[1], d2 = midi->data[2];

        SfizzEngine::MidiEvent ev{};
        ev.delaySamples = delay;
        ev.channel = midi->data[0] & 0x0F;
        if (status == 0x90 && d2 > 0) {
            ev.type = SfizzEngine::MidiEvent::Type::NoteOn;
            ev.noteNumber = d1;
            ev.velocity = d2;
            noteOnVisual(d1, d2);
        } else if (status == 0x80 || (status == 0x90 && d2 == 0)) {
            ev.type = SfizzEngine::MidiEvent::Type::NoteOff;
            ev.noteNumber = d1;
            ev.velocity = d2;
            noteOffVisual(d1, d2);
        } else if (status == 0xB0) {
            ev.type = SfizzEngine::MidiEvent::Type::CC;
            ev.ccNumber = d1;
            ev.ccValue = d2;
            if (d1 == 7) p->params.ccVolume.store(d2);
            else if (d1 == 10) p->params.ccPan.store(d2);
        } else if (status == 0xE0) {
            ev.type = SfizzEngine::MidiEvent::Type::PitchWheel;
            ev.pitch = ((d2 << 7) | d1) - 8192;
        } else {
            continue; // aftertouch/program-change/etc. not handled
        }
        events[numEvents++] = ev;
    }

    p->engine.renderBlock(events, static_cast<int>(numEvents), ob.data32,
                          static_cast<int>(ob.channel_count), static_cast<int>(process->frames_count),
                          p->params.mpeEnabled.load(), p->params.tuningFrequency.load());
    p->params.activeVoiceCount = p->engine.activeVoiceCount();
    return CLAP_PROCESS_CONTINUE;
}

const void* plugGetExtension(const clap_plugin_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kExtAudioPorts;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kExtNotePorts;
    if (!strcmp(id, CLAP_EXT_GUI)) return &kExtGui;
    if (!strcmp(id, CLAP_EXT_STATE)) return &kExtState;
    return nullptr;
}

void plugOnMainThread(const clap_plugin_t*) {}

// ----------------------------------------------------------------- factory

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*,
                                   const clap_host_t* host,
                                   const char* plugin_id) {
    if (strcmp(plugin_id, kDesc.id) != 0) return nullptr;
    Plugin* p = new (std::nothrow) Plugin(host);
    if (!p) return nullptr;
    p->plug.desc = &kDesc;
    p->plug.plugin_data = p;
    p->plug.init = plugInit;
    p->plug.destroy = plugDestroy;
    p->plug.activate = plugActivate;
    p->plug.deactivate = plugDeactivate;
    p->plug.start_processing = plugStartProcessing;
    p->plug.stop_processing = plugStopProcessing;
    p->plug.reset = plugReset;
    p->plug.process = plugProcess;
    p->plug.get_extension = plugGetExtension;
    p->plug.on_main_thread = plugOnMainThread;
    return &p->plug;
}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDesc(const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &kDesc : nullptr;
}

const clap_plugin_factory_t kFactory = {
    .get_plugin_count = factoryCount,
    .get_plugin_descriptor = factoryDesc,
    .create_plugin = factoryCreate,
};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* id) {
    if (!strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entryInit,
    .deinit = entryDeinit,
    .get_factory = entryGetFactory,
};
