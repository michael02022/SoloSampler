#pragma once

#include <string>
#include <vector>

// Parameters that drive the in-RAM SFZ text: a `<global>` header carrying
// every opcode except `sample=`, followed by one `<region>` per stacked item
// (see stack below). Opcode names match ExplicacionPlugin.txt exactly
// (legacy SFZ 1.0 spellings: bendup/benddown/loopmode, not the v2
// underscored forms).
//
// The virtual sfz "path" sfizz is told about is always "/" (see
// plugin.cpp's regenerateAndLoadSfz), so sample= must carry the sample's
// full absolute path with the leading '/' stripped - not a bare filename.
struct SfzRegionParams {
    // Sample tab's "stack" (see GuiState::StackItem in shared.hpp): each
    // entry is either a bare region (sampleRelativePath) or a pre-flattened
    // .sfz mapping (regionsText, from SfzFlatten.h) - emitRegion() in
    // buildSfzText() concatenates all of them, in order, every place a
    // single region used to go, including being duplicated per "oscillator"
    // <group> the same way a single region already was when the FX tab's
    // fxEnabled is on. Empty stack emits one bare, sample-less <region> (the
    // "nothing loaded yet" placeholder the engine still needs to parse).
    struct StackRegion {
        bool isSfz = false;
        std::string sampleRelativePath; // !isSfz only
        std::string regionsText;        // isSfz only
    };
    std::vector<StackRegion> stack;

    // "Character" (Sample tab, only editable once the stack contains an
    // .sfz - see sfzlab's sfzlink module, src/modules/sfzlink.py, this
    // replicates its exact note_offset=/transpose= pattern): a -12..12
    // semitone shift applied to the whole imported mapping. Written
    // unconditionally (a no-op at its default 0) - same "whatever's in the
    // control gets picked up" convention as the rest of this struct.
    int character = 0;
    // Piano right-click's multisample-mode root note (default/reset 60) -
    // see SharedParams::multisampleRootNote for why. (multisampleRootNote -
    // 60) is added into transpose= on top of character, unconditionally
    // (see buildSfzText) - this field's default is a true no-op, so leaving
    // it untouched never affects plain-sample playback either way, and
    // single-sample mode's pitch_keycenter= still gets its own dedicated
    // rootNote below regardless.
    int multisampleRootNote = 60;
    int bendUpCents = 2400;
    int bendDownCents = -2400;
    int quality = 2;
    std::string loopMode = "none"; // sentinel: skip loopmode=, let sfizz decide (see buildSfzText)
    int polyphony = 256;
    int notePolyphony = 256;
    float tuneCents = 0.0f;
    int rootNote = 60;
    int volume = 0; // -48..48 (volume=)
    bool offsetEnabled = false;
    int offsetValue = 0;
    bool randomOffsetEnabled = false; // writes offset_oncc135=
    int randomOffsetValue = 0;        // max(0, maxOffset - offsetValue)

    // Pan tab: panRandom writes pan_oncc136= always + pan_oncc135=/137=
    // (toggled by panAlternate) with the same value. lfo01_* is the Pan LFO.
    int panRandom = 0;
    bool panAlternate = false;
    // "x2" - see SharedParams::panX2's comment. Doubles pan_oncc136=/137=,
    // lfo01_pan=, and Opcode FX's pan_oncc117=/hard pan= values, plus adds
    // pan_oncc10=200 - see buildSfzText's panMul.
    bool panX2 = false;
    float panLfoDelay = 0.0f;
    float panLfoFade = 0.0f;
    int panLfoPan = 0;
    float panLfoFreq = 10.0f;
    int panLfoWave = 1;

    // Amp tab: fixed 6-breakpoint eg01_* envelope + lfo02_* (amp LFO).
    // See buildSfzText() for the literal opcode template these fill in.
    int ampKeycenter = 60;
    int ampKeytrack = 0;
    int ampVeltrack = 95;
    int ampRandom = 0; // writes volume_oncc135=

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

    // Filter tab.
    std::string filterType = "lpf_2p";
    int filterCutoff = 20000;
    float filterResonance = 0.0f;
    int filterRandomCutoff = 0;
    int filKeycenter = 60;
    int filKeytrack = 0;
    int filVeltrack = 0;    // cutoff_oncc131= or eg02_cutoff_oncc131=
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

    // Pitch tab.
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
    float pitchLfoFreq = 4.5f;
    int pitchLfoWave = 1;

    // Opcodes tab: raw user-typed text, written verbatim after every other
    // <global> opcode (see buildSfzText). Free-form escape hatch for
    // anything the UI doesn't expose.
    std::string customOpcodes = "//custom opcodes here\nlfo04_pitch_oncc1=40";

    // FX tab ("Opcode FX") - see buildSfzText for the per-mode <group>/
    // <region> layout this drives.
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
    bool fxIndependentLfo = false; // Chorus Stereo modes: 2nd osc uses lfo06 instead of lfo05

    // Opcode FX's second filter (fil2type=/cutoff2=) - see buildSfzText for
    // exactly which wet oscillator(s) it's written into. fil2Type is
    // already resolved to its opcode string, same convention as filterType.
    std::string fil2Type = "lpf_2p";
    int cutoff2 = 11700;

    // Opcode FX's makeup gain (volume=sample_volume+fxVolume, both dB
    // values that stack, written into the same wet oscillator(s) as
    // fil2Type/cutoff2 above) - see buildSfzText.
    int fxVolume = -6;

    // FX tab, real reverb (sfizz's built-in <effect> type=fverb block - see
    // buildSfzText, written between <control> and <global>). reverbType is
    // already resolved to its opcode string (see kReverbTypeNames in
    // shared.hpp), same convention as filterType/loopMode above.
    bool reverbEnabled = false;
    std::string reverbType = "chamber";
    float reverbInput = 100.0f;
    float reverbPredelay = 50.0f;
    float reverbSize = 50.0f;
    float reverbTone = 50.0f;
    float reverbDamp = 50.0f;
    float reverbDry = 100.0f;
    float reverbWet = 100.0f;
};

std::string buildSfzText(const SfzRegionParams& params);
