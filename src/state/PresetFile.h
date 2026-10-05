// .sspreset / .ssprofile file format - local-only save/recall of an
// instrument design, independent of (and NOT wire-compatible with) the CLAP
// host state format in plugin.cpp's stateSave/stateLoad. Plugin-agnostic:
// this file only knows about plain values, never clap_plugin_t/SharedParams
// directly - plugin.cpp's saveSoloSamplerPreset/loadSoloSamplerPreset (and
// applyPresetFields) are the glue that connects this to a running instance.
#pragma once

#include <string>
#include <vector>

#include "midi/ExternalSettings.h"

// One entry of the Sample tab's load stack, as persisted in a .sspreset/CLAP
// host state - mirrors shared.hpp's GuiState::StackItem but only the fields
// that need to survive a save (sampleRelativePath/regionCount are re-derived
// on load, see plugin.cpp's restoreStack).
struct PresetStackItem {
    std::string path;
    bool isSfz = false;
    std::string regionsText; // isSfz only
};

// Every field a .sspreset/.ssprofile can carry. stack is the "sample
// identity" - only present in a kind==0 (preset) file, always empty when
// read back from a kind==1 (profile) file (see readPresetFile). Field
// names/types/defaults mirror plugin.cpp's stateLoad locals 1:1 - keep both
// in sync when adding a new instrument parameter (see that function's own
// comment on the established multi-file pattern for new params).
struct PresetFields {
    int rootNote = 60;
    int volume = 0;
    int bendUpCents = 2400;
    int bendDownCents = -2400;
    int quality = 2;
    int loopModeIndex = 4; // "none"
    int polyphony = 256;
    int notePolyphony = 256;
    float tuneCents = 0.0f;
    bool offsetEnabled = false;
    int offsetValue = 0;
    bool randomOffsetEnabled = false;
    int panRandom = 0;
    bool panAlternate = false;
    float panLfoDelay = 0.0f;
    float panLfoFade = 0.0f;
    int panLfoPan = 0;
    float panLfoFreq = 10.0f;
    int panLfoWave = 1;
    int ampKeycenter = 60;
    int ampKeytrack = 0;
    int ampVeltrack = 95;
    int ampRandom = 0;
    float ampStartLevel = 0.0f;
    float ampDelayTime = 0.00001f;
    float ampAttackTime = 0.00001f;
    float ampAttackShape = 0.00001f;
    float ampHoldTime = 0.00001f;
    float ampDecayTime = 0.00001f;
    float ampDecayShape = -0.3616f;
    float ampSustainLevel = 1.0f;
    float ampReleaseTime = 0.00001f;
    float ampReleaseShape = -6.3616f;
    float ampLfoDelay = 0.0f;
    float ampLfoFade = 0.0f;
    float ampLfoVolume = 0.0f;
    float ampLfoFreq = 10.0f;
    int ampLfoWave = 1;
    int filterTypeIndex = 1;
    int filterCutoff = 20000;
    float filterResonance = 0.0f;
    int filterRandomCutoff = 0;
    int filKeycenter = 60;
    int filKeytrack = 0;
    int filVeltrack = 0;
    int resoVeltrack = 0;
    bool filterEgEnabled = false;
    int filDepth = 0;
    float filEgStartLevel = 0.0f;
    float filEgDelayTime = 0.00001f;
    float filEgAttackTime = 0.00001f;
    float filEgAttackShape = 0.00001f;
    float filEgHoldTime = 0.00001f;
    float filEgDecayTime = 0.00001f;
    float filEgDecayShape = 0.00001f;
    float filEgSustainLevel = 1.0f;
    float filEgReleaseTime = 0.00001f;
    float filEgReleaseShape = 0.00001f;
    float filterLfoDelay = 0.0f;
    float filterLfoFade = 0.0f;
    int filterLfoDepth = 0;
    float filterLfoFreq = 10.0f;
    int filterLfoWave = 1;
    int pitchKeytrack = 100;
    int pitchVeltrack = 0;
    int pitchRandom = 0;
    bool portamentoEnabled = false;
    float glideTime = 0.0f;
    bool pitchEgEnabled = false;
    int pitchDepth = 0;
    float pitchEgStartLevel = 0.0f;
    float pitchEgDelayTime = 0.00001f;
    float pitchEgAttackTime = 0.00001f;
    float pitchEgAttackShape = 0.00001f;
    float pitchEgHoldTime = 0.00001f;
    float pitchEgDecayTime = 0.00001f;
    float pitchEgDecayShape = 0.00001f;
    float pitchEgSustainLevel = 1.0f;
    float pitchEgReleaseTime = 0.00001f;
    float pitchEgReleaseShape = 0.00001f;
    float pitchLfoDelay = 0.0f;
    float pitchLfoFade = 0.0f;
    int pitchLfoPitch = 0;
    float pitchLfoFreq = 10.0f;
    int pitchLfoWave = 1;
    int ccVolume = 100;
    int ccPan = 64;
    bool fxEnabled = false;
    int fxMode = 0;
    int fxDetune = 15;
    int fxDetuneCcMode = 0;
    float fxDelay = 0.05f;
    int fxDelayCcMode = 1;
    int fxStereoWidth = 0;
    int fxDepth = -15;
    float fxSpeed = 0.5f;
    int fxWave = 0;
    float fxPhase = 0.5f;
    int fxPhaseCcMode = 1;
    bool fxIndependentLfo = false;
    int fil2TypeIndex = 1;
    int cutoff2 = 11700;
    int fxVolume = -6;
    bool reverbEnabled = false;
    int reverbTypeIndex = 0;
    float reverbInput = 100.0f;
    float reverbPredelay = 50.0f;
    float reverbSize = 50.0f;
    float reverbTone = 50.0f;
    float reverbDamp = 50.0f;
    float reverbDry = 100.0f;
    float reverbWet = 100.0f;
    // External tab - stored as a trailing, size-prefixed blob after
    // everything else (see writeExternalBlob), so files written before it
    // existed still load, with these left at their defaults.
    ExternalSettings external;
    std::string customOpcodesText;
    bool mpeEnabled = false;
    int character = 0;
    int multisampleRootNote = 60;
    bool panX2 = false;

    // Sample identity - kind==0 (preset) only, see the file-level comment.
    std::vector<PresetStackItem> stack;
};

// kind: 0 = preset (.sspreset, writes the sample-identity fields too),
// 1 = profile (.ssprofile, omits them entirely from the file).
bool writePresetFile(const std::string& path, int kind, const PresetFields& fields);

struct PresetFileResult {
    bool ok = false;
    std::string error;
    int kind = 0;
    PresetFields fields;
};

// kind==1 results always have default/empty sample-identity fields (the
// bytes simply aren't in the file) - callers decide whether to apply them
// based on their own applySampleData flag, not on kind alone (see
// plugin.cpp's loadSoloSamplerPreset).
PresetFileResult readPresetFile(const std::string& path);
