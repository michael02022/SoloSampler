// State shared between the audio thread and the GUI thread. Every control is
// a plain atomic - no clap_plugin_params_t, matching NeoLooper (see project
// plan/memory for why): DAW automation of these specific controls is out of
// scope, full UI editability is unaffected.
//
// Defaults/ranges here match the old JUCE+JIVE version exactly (see
// PluginProcessor.cpp on the main branch checkpoint) so the port doesn't
// silently change behavior a user already relied on.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// SFZ loop-mode opcode strings, indexed by SharedParams::loopModeIndex.
// Matches the old JUCE AudioParameterChoice exactly (order and default
// index 1 = "one_shot" both matter - don't reorder without updating
// loopModeIndex's default below). "none" (index 4, appended rather than
// inserted so existing saved-state indices keep their meaning) is a
// sentinel, not a real SFZ value - buildSfzText skips writing loopmode=
// entirely when this is selected, leaving sfizz to decide for itself
// (its own default: loop if the sample has embedded loop points, one-shot
// otherwise) instead of forcing one.
inline constexpr const char* kLoopModes[] = {"no_loop", "one_shot", "loop_continuous",
                                             "loop_sustain", "none"};

// lfoNN_wave opcode values 0-7 (Pan=lfo01, Amp=lfo02, Filter=lfo03,
// Pitch=lfo04), user-specified names for this project's UI.
inline constexpr const char* kLfoWaveNames[8] = {
    "triangle", "sine", "75% pulse", "square (50% pulse)",
    "25% pulse", "12.5% pulse", "saw up", "saw down"};

// filtype opcode values, indexed by SharedParams::filterTypeIndex. The
// standard SFZ v2/ARIA filter type list - default index 1 = "lpf_2p".
inline constexpr const char* kFilterTypes[23] = {
    "lpf_1p", "lpf_2p", "lpf_2p_sv", "lpf_4p", "lpf_6p",
    "hpf_1p", "hpf_2p", "hpf_2p_sv", "hpf_4p", "hpf_6p",
    "bpf_1p", "bpf_2p", "bpf_2p_sv",
    "brf_1p", "brf_2p", "brf_2p_sv",
    "apf_1p", "pkf_2p",
    "comb", "pink", "lsh", "hsh", "peq"};

// FX tab "Opcode FX" mode names, indexed by SharedParams::fxMode - see
// buildSfzText for the <group>/<region> layout each mode generates.
inline constexpr const char* kSampleFxModeNames[4] = {
    "Unison", "Chorus Mono", "Chorus Stereo (Wet)", "Chorus Stereo (Wet+Dry)"};

// "CC MODE" combobox names for the FX tab's Detune/Delay/FX Phase sliders -
// index 0 ("None") uses that opcode's own dedicated default CC (set to 127
// in the generated <control> block, see buildSfzText), indices 1-3 redirect
// to the shared cc135/136/137 modulation slots used elsewhere in this UI
// (Pan/Amp/Filter Random).
inline constexpr const char* kFxCcModeNames[4] = {"None", "CC135", "CC136", "CC137"};

// Real FX tab reverb type names (sfizz's built-in <effect> type=fverb
// effect, reverb_type= opcode), indexed by SharedParams::reverbTypeIndex.
inline constexpr const char* kReverbTypeNames[7] = {
    "chamber", "large_hall", "large_room", "mid_hall",
    "mid_room", "small_hall", "small_room"};

struct SharedParams {
    std::atomic<int> bendUpCents{2400};
    std::atomic<int> bendDownCents{-2400};
    std::atomic<int> quality{2}; // 0-10
    std::atomic<int> loopModeIndex{4}; // "none", see kLoopModes above
    std::atomic<int> polyphony{256}; // 1-1024
    std::atomic<int> notePolyphony{256}; // 1-1024
    // Engine-level MPE (MIDI Polyphonic Expression) toggle - sfizz routes
    // each MIDI channel's pitch bend/CC/aftertouch independently per voice
    // when on (see sfizz.h's MPE group / sfizz_set_mpe_enabled). Applied on
    // the audio thread (SfizzEngine::renderBlock, RT-thread-only per
    // sfizz.h) from this atomic read fresh every block - not an SFZ opcode,
    // so toggling it does not trigger onParamChanged/regenerateAndLoadSfz.
    std::atomic<bool> mpeEnabled{false};
    // Pitch tab's "Tuning" section: concert-pitch A4 reference in Hz
    // (standard default 440), applied via sfizz_set_tuning_frequency - RT-
    // thread-only per sfizz.h (same convention as mpeEnabled above), so
    // toggling it does not trigger onParamChanged/regenerateAndLoadSfz
    // either. Deliberately NOT part of PresetFields/.sspreset/.ssprofile -
    // this is a DAW-session-only setting (persisted in CLAP host state,
    // see plugin.cpp's stateSave/stateLoad, but not in the portable preset
    // formats) per explicit user direction.
    std::atomic<float> tuningFrequency{440.0f};
    std::atomic<float> tuneCents{0.0f}; // -100..100
    std::atomic<int> rootNote{60};
    std::atomic<int> volume{0}; // -48..48 (volume=)
    std::atomic<bool> offsetEnabled{false};
    std::atomic<int> offsetValue{0};
    // "Character" (Sample tab): -12..12 semitone shift over the whole
    // imported multisample mapping, writing note_offset= (under <control>)
    // - replicates sfzlab's sfzlink module (src/modules/sfzlink.py) exactly.
    // transpose= (in <global>) is character PLUS the piano-derived offset
    // below unconditionally - see buildSfzText. The slider is only
    // interactive once the Sample tab's stack (GuiState::stack) contains a
    // real multisample mapping (see drawSampleTab's hasSfzInStack) but the
    // value itself isn't cleared/gated when the stack changes.
    std::atomic<int> character{0}; // -12..12
    // Keeps the piano's right-click "set root note" gesture useful once the
    // stack contains a real multisample mapping: pitch_keycenter= there is
    // normally driven per-region by the imported mapping itself (see
    // GuiState::StackItem::regionsText), so overwriting the plugin's own
    // global rootNote/pitch_keycenter= on right-click - like a plain-sample
    // stack does - would have no audible effect (region-level opcodes win).
    // Instead, while the stack has an .sfz in it, right-clicking the piano
    // stores the clicked note here and buildSfzText adds
    // (multisampleRootNote - 60) into transpose= on top of character (a
    // true no-op at the default 60, so it's added unconditionally rather
    // than gated), so the click acts as a relative nudge rather than an
    // absolute pitch_keycenter override. Reset to 60 by plugin.cpp's
    // loadStack whenever an .sfz becomes the stack's sole item - the
    // piano's root marker and the transpose contribution both start
    // neutral for a freshly imported mapping.
    std::atomic<int> multisampleRootNote{60};
    // "Random" offset (writes offset_oncc135=): the value isn't
    // independently stored - it's always max(0, maxOffset - offsetValue)
    // (see plugin.cpp's regenerateAndLoadSfz), recomputed fresh every time
    // so it can never go stale relative to offsetValue/the loaded sample's
    // frame count.
    std::atomic<bool> randomOffsetEnabled{false};

    // Pan tab: panRandom writes BOTH pan_oncc136= (always) and pan_oncc135=
    // (or pan_oncc137= when panAlternate) with the SAME value - two
    // independent random-pan CC slots per user spec. lfo01_* is the Pan
    // LFO - LFO opcode numbering across the whole plugin is Pan=01, Amp=02,
    // Filter=03, Pitch=04 (renumbered per explicit user correction; EG
    // numbering is unaffected - Amp=eg01, Filter=eg02, Pitch=eg03,
    // Portamento=eg04, unchanged).
    std::atomic<int> panRandom{0};        // -200..200 (pan_oncc136=/135=/137=)
    std::atomic<bool> panAlternate{false}; // pan_oncc135= <-> pan_oncc137=
    // "x2" (Pan tab, default off, checkbox at the top of the tab): writes
    // pan_oncc10=200 (doubles the pan range for a plain MIDI CC10 pan
    // message) and doubles every OTHER pan-opcode value this file writes
    // (Pan Random, Pan LFO, Opcode FX's stereo-width/hard pan=) - for
    // instruments built from hard-panned mono samples (one mic per side)
    // where the normal pan range can't bring a channel back across center.
    // See SfzDocument.cpp's buildSfzText/panMul.
    std::atomic<bool> panX2{false};

    std::atomic<float> panLfoDelay{0.0f}; // 0..4       (lfo01_delay)
    std::atomic<float> panLfoFade{0.0f};  // 0..4       (lfo01_fade)
    std::atomic<int> panLfoPan{0};        // -200..200  (lfo01_pan)
    std::atomic<float> panLfoFreq{10.0f}; // -20..20    (lfo01_freq)
    std::atomic<int> panLfoWave{1};       // 0..7       (lfo01_wave)

    // Amp tab: keycenter/keytrack/veltrack/random opcodes, eg01_* (a fixed
    // 6-breakpoint envelope template - see SfzDocument.cpp's buildSfzText
    // for the literal opcode layout), and lfo02_* (amp LFO). Defaults/
    // ranges are the user-specified ones for this project, not sfizz's own
    // opcode defaults.
    std::atomic<int> ampKeycenter{60};  // 0..127   (amp_keycenter=)
    std::atomic<int> ampKeytrack{0};    // -96..12  (amp_keytrack=)
    std::atomic<int> ampVeltrack{95};  // -100..100 (amp_veltrack=)
    std::atomic<int> ampRandom{0};      // -24..24  (volume_oncc135=)

    std::atomic<float> ampStartLevel{0.0f};     // 0..1        (eg01_level0/1)
    std::atomic<float> ampDelayTime{0.00001f};  // 0.00001..4  (eg01_time1)
    std::atomic<float> ampAttackTime{0.00001f}; // 0.00001..8  (eg01_time2)
    std::atomic<float> ampAttackShape{0.00001f}; // -11..11    (eg01_shape2)
    std::atomic<float> ampHoldTime{0.00001f};   // 0.00001..8  (eg01_time3)
    std::atomic<float> ampDecayTime{0.00001f};  // 0.00001..20  (eg01_time4)
    std::atomic<float> ampDecayShape{-0.3616f}; // -11..11     (eg01_shape4)
    std::atomic<float> ampSustainLevel{1.0f};   // 0..1        (eg01_level4)
    std::atomic<float> ampReleaseTime{0.00001f}; // 0.00001..8 (eg01_time5)
    std::atomic<float> ampReleaseShape{-6.3616f}; // -11..11   (eg01_shape5)

    std::atomic<float> ampLfoDelay{0.0f};  // 0..4    (lfo02_delay)
    std::atomic<float> ampLfoFade{0.0f};   // 0..4    (lfo02_fade)
    std::atomic<float> ampLfoVolume{0.0f}; // -20..20 (lfo02_volume)
    std::atomic<float> ampLfoFreq{10.0f};  // -20..20 (lfo02_freq)
    std::atomic<int> ampLfoWave{1};        // 0..7    (lfo02_wave), see kLfoWaveNames

    // Filter tab.
    std::atomic<int> filterTypeIndex{1};     // "lpf_2p", see kFilterTypes above (filtype=)
    std::atomic<int> filterCutoff{20000};    // 1..20000            (cutoff=)
    std::atomic<float> filterResonance{0.0f}; // -40..40 dB         (resonance=)
    std::atomic<int> filterRandomCutoff{0};  // 1..20000            (cutoff_oncc135=)
    std::atomic<int> filKeycenter{60};       // 0..127              (fil_keycenter=)
    std::atomic<int> filKeytrack{0};         // 0..1200             (fil_keytrack=)
    // cutoff_oncc131= normally, or eg02_cutoff_oncc131= when
    // filterEgEnabled (see regenerateAndLoadSfz/buildSfzText) - same value,
    // different opcode name depending on whether the filter EG is active.
    std::atomic<int> filVeltrack{0};         // 0..20000
    std::atomic<int> resoVeltrack{0};        // -40..40             (resonance_oncc131=)

    // Filter EG (eg02_*, same fixed 6-breakpoint template as the Amp tab's
    // eg01_*, plus eg02_cutoff=filDepth) - gated behind filterEgEnabled
    // (user-requested checkbox; unlike the Amp EG, which is always on).
    std::atomic<bool> filterEgEnabled{false};
    std::atomic<int> filDepth{0};              // 0..20000          (eg02_cutoff)
    std::atomic<float> filEgStartLevel{0.0f};
    std::atomic<float> filEgDelayTime{0.00001f};
    std::atomic<float> filEgAttackTime{0.00001f};
    std::atomic<float> filEgAttackShape{0.00001f};
    std::atomic<float> filEgHoldTime{0.00001f};
    std::atomic<float> filEgDecayTime{0.00001f};
    std::atomic<float> filEgDecayShape{0.00001f};
    std::atomic<float> filEgSustainLevel{1.0f};
    std::atomic<float> filEgReleaseTime{0.00001f};
    std::atomic<float> filEgReleaseShape{0.00001f};

    // Filter LFO (lfo03_*) - same defaults/ranges as the Amp LFO except
    // lfo03_cutoff (filterLfoDepth) replaces lfo02_volume's slot.
    std::atomic<float> filterLfoDelay{0.0f};  // 0..4      (lfo03_delay)
    std::atomic<float> filterLfoFade{0.0f};   // 0..4      (lfo03_fade)
    std::atomic<int> filterLfoDepth{0};       // -20000..20000 (lfo03_cutoff)
    std::atomic<float> filterLfoFreq{10.0f};  // -20..20   (lfo03_freq)
    std::atomic<int> filterLfoWave{1};        // 0..7      (lfo03_wave)

    // Pitch tab.
    std::atomic<int> pitchKeytrack{100}; // -1200..1200   (pitch_keytrack=)
    std::atomic<int> pitchVeltrack{0};   // -9600..9600   (pitch_veltrack=)
    std::atomic<int> pitchRandom{0};     // 0..9600       (pitch_random=)

    // Portamento (eg04_*, a fixed 2-breakpoint glide template - see
    // buildSfzText for the literal opcode layout, only eg04_time1/glideTime
    // is user-configurable, everything else in the template is a fixed
    // literal per the SFZ portamento-via-envelope convention) - gated
    // behind portamentoEnabled.
    std::atomic<bool> portamentoEnabled{false};
    std::atomic<float> glideTime{0.0f}; // 0..2 (eg04_time1)

    // Pitch EG (eg03_*, same fixed 6-breakpoint template as Amp/Filter,
    // plus eg03_pitch=pitchDepth) - gated behind pitchEgEnabled, same
    // checkbox-gated convention as the Filter EG. Shape defaults are
    // 0.00001 (linear) here, NOT Amp's -0.3616/-6.3616 - those decay/
    // release "curved by default" values are specific to Amp; every other
    // EG in this project (Filter, Pitch) defaults every shape to linear.
    std::atomic<bool> pitchEgEnabled{false};
    std::atomic<int> pitchDepth{0};        // -9600..9600  (eg03_pitch)
    std::atomic<float> pitchEgStartLevel{0.0f};
    std::atomic<float> pitchEgDelayTime{0.00001f};
    std::atomic<float> pitchEgAttackTime{0.00001f};
    std::atomic<float> pitchEgAttackShape{0.00001f};
    std::atomic<float> pitchEgHoldTime{0.00001f};
    std::atomic<float> pitchEgDecayTime{0.00001f};
    std::atomic<float> pitchEgDecayShape{0.00001f};
    std::atomic<float> pitchEgSustainLevel{1.0f};
    std::atomic<float> pitchEgReleaseTime{0.00001f};
    std::atomic<float> pitchEgReleaseShape{0.00001f};

    // Pitch LFO (lfo04_*) - same defaults/ranges as the Filter LFO except
    // lfo04_pitch (pitchLfoPitch) replaces lfo03_cutoff's slot.
    std::atomic<float> pitchLfoDelay{0.0f}; // 0..4       (lfo04_delay)
    std::atomic<float> pitchLfoFade{0.0f};  // 0..4       (lfo04_fade)
    std::atomic<int> pitchLfoPitch{0};      // -2400..2400 (lfo04_pitch)
    std::atomic<float> pitchLfoFreq{4.5f}; // -20..20    (lfo04_freq)
    std::atomic<int> pitchLfoWave{1};       // 0..7       (lfo04_wave)

    // FX tab ("Opcode FX"): when fxEnabled, the single <region> normally
    // emitted at the end of buildSfzText is replaced by 2-3 <group>s (one
    // per "oscillator") that all repeat the SAME loaded sample with
    // different delay/detune/pan/chorus-LFO opcodes (lfo05_*) - see
    // buildSfzText's Opcode FX section for the exact per-mode layout.
    // fxDetune/fxDelay/fxPhase each pick their own modulation CC via a
    // "CC MODE" combobox (kFxCcModeNames): index 0 uses that opcode's own
    // dedicated default CC (90/89/116 respectively), 1-3 redirect to the
    // shared cc135/136/137 slots.
    std::atomic<bool> fxEnabled{false};
    std::atomic<int> fxMode{0};          // see kSampleFxModeNames
    std::atomic<int> fxDetune{15};       // -100..100 (tune_oncc<N>=)
    std::atomic<int> fxDetuneCcMode{0};  // see kFxCcModeNames - default cc90
    std::atomic<float> fxDelay{0.05f};   // 0..0.1    (delay_oncc<N>=)
    std::atomic<int> fxDelayCcMode{1};   // see kFxCcModeNames - default cc89, defaults to CC135
    std::atomic<int> fxStereoWidth{0};   // 0..100    (pan_oncc117=), Unison mode only
    // Chorus-only extras (every mode except Unison).
    std::atomic<int> fxDepth{-15};     // -100..100 (lfo05_pitch=)
    std::atomic<float> fxSpeed{0.5f};  // 0..10     (lfo05_freq=)
    std::atomic<int> fxWave{0};        // 0..7      (lfo05_wave=), see kLfoWaveNames - triangle
    std::atomic<float> fxPhase{0.5f};  // 0..1      (lfo05_phase_oncc<N>=)
    std::atomic<int> fxPhaseCcMode{1}; // see kFxCcModeNames - default cc116, defaults to CC135
    // Chorus Stereo (Wet/Wet+Dry) only: normally both oscillators share
    // lfo05 (same depth/speed/wave/phase, so a synced chorus); when set, the
    // second oscillator uses lfo06 instead, so the two run fully
    // independently (still same depth/speed/wave/phase values, just a
    // separate LFO instance) - see buildSfzText's emitChorusStereoPair.
    std::atomic<bool> fxIndependentLfo{false};

    // Opcode FX's second filter (fil2type=/cutoff2=): applied to the wet
    // oscillator(s) only - Chorus Mono's oscillator 1, and both oscillators
    // of Chorus Stereo (Wet+Dry) (NOT Chorus Stereo (Wet), which has no
    // separate dry oscillator to contrast against - see buildSfzText's
    // emitChorusStereoPair). Same option list/index convention as the main
    // Filter tab's filterTypeIndex/kFilterTypes.
    std::atomic<int> fil2TypeIndex{1}; // "lpf_2p", see kFilterTypes (fil2type=)
    std::atomic<int> cutoff2{11700};   // 1..20000 (cutoff2=)

    // Opcode FX's makeup gain, written into the same wet oscillator(s) as
    // fil2type=/cutoff2= above (volume=sample_volume+fxVolume, see
    // buildSfzText - both are dB values that simply stack/add) - compensates
    // for the perceived loudness/comb-filtering change from summing the
    // chorus/detuned copy against the dry one.
    std::atomic<int> fxVolume{-6}; // -48..0 (added to Sample tab's volume=)

    // FX tab, real reverb: sfizz's built-in <effect> type=fverb block (see
    // buildSfzText) - unlike the "Opcode FX" section above, this is an
    // actual DSP effect, not opcode-emulated via <group>/<region>
    // repetition, and its <effect> header must sit between <control> and
    // <global> rather than after the <region>(s). reverbEnabled gates
    // whether the <effect> block is written at all (default off).
    std::atomic<bool> reverbEnabled{false};
    std::atomic<int> reverbTypeIndex{0};      // "chamber", see kReverbTypeNames (reverb_type=)
    std::atomic<float> reverbInput{100.0f};   // 0..100 (reverb_input=)
    std::atomic<float> reverbPredelay{50.0f}; // 0..100 (reverb_predelay=)
    std::atomic<float> reverbSize{50.0f};     // 0..100 (reverb_size=)
    std::atomic<float> reverbTone{50.0f};     // 0..100 (reverb_tone=)
    std::atomic<float> reverbDamp{50.0f};     // 0..100 (reverb_damp=)
    std::atomic<float> reverbDry{100.0f};     // 0..100 (reverb_dry=)
    std::atomic<float> reverbWet{100.0f};     // 0..100 (reverb_wet=)

    // CC7 (volume) / CC10 (pan), 0-127. Two directions share these atomics:
    // the audio thread updates them on incoming MIDI CC (so the GUI can
    // display what an external controller is sending), and the GUI thread
    // updates them when the user drags a slider. pendingCc* is the
    // lock-free GUI->audio handoff: the GUI thread sets the flag after
    // writing a new value, the audio thread drains it at the top of
    // process() and injects a real CC event before rendering.
    std::atomic<int> ccVolume{100};
    std::atomic<int> ccPan{64};
    std::atomic<bool> pendingCcVolume{false};
    std::atomic<bool> pendingCcPan{false};

    // MIDI monitor for the Debug tab: a small lock-free overwrite ring
    // (audio thread writes via fetch_add, GUI thread reads the array
    // directly) - matches the old JUCE version's approach exactly,
    // including tolerating the minor read/write race on a given slot
    // (acceptable for a non-critical debug display, avoids a mutex on the
    // audio thread).
    struct MidiLogEntry {
        bool isNoteOn = false;
        int channel = 0;
        int noteNumber = 0;
        int velocity = 0;
    };
    static constexpr size_t kMidiLogCapacity = 24;
    std::array<MidiLogEntry, kMidiLogCapacity> midiLog;
    std::atomic<uint32_t> midiLogWriteCount{0};

    // Piano keyboard visualization: mirrors every note the engine is
    // currently sounding (real incoming MIDI and GUI-preview clicks alike,
    // both go through the same process()-side note handling) so the GUI can
    // highlight active keys with a velocity-proportional bar - matches the
    // old JUCE MidiKeyboardComponent behavior. Audio thread writes, GUI
    // thread only reads.
    std::array<std::atomic<bool>, 128> noteActive{};
    std::array<std::atomic<float>, 128> noteVelocity01{};

    // Live sfizz voice count (SfizzEngine::activeVoiceCount, mirrored here
    // every block in plugProcess) - shown as a small counter under the
    // piano (see drawTuneAndPiano). Audio thread writes, GUI thread reads.
    std::atomic<int> activeVoiceCount{0};

    // GUI -> audio preview-note handoff (clicking the on-screen piano),
    // same pending-flag pattern as the CC handoff above.
    std::atomic<bool> pendingPreviewNoteOn{false};
    std::atomic<int> previewNoteOnNumber{0};
    std::atomic<int> previewNoteOnVelocity{0};
    std::atomic<bool> pendingPreviewNoteOff{false};
    std::atomic<int> previewNoteOffNumber{0};

    // GUI-thread-only data (file path, last generated SFZ text, sample
    // analysis) - never touched by the audio thread, so a plain mutex is
    // fine here (no RT-safety concern).
    struct GuiState {
        std::mutex mutex;

        // Sample tab's "stack": the file(s) most recently loaded together in
        // one explicit pick/drop (plugin.cpp's loadStack() - a single file
        // replaces this whole vector, same as loading always worked before
        // this feature; 2+ files picked/dropped at once replace it with all
        // of them). buildSfzText (via regenerateAndLoadSfz) concatenates
        // every item here so they all play together as a layer - the stack
        // never grows via separate/sequential loads, only via one multi-file
        // selection. A plain sample carries
        // just sampleRelativePath (root-relative, ready for a bare
        // sample=<...> region); an .sfz carries regionsText, the pre-
        // flattened/cleaned/path-rewritten output of flattenMultisampleSfz()
        // - persisted verbatim in plugin state so a saved project survives
        // the source .sfz being moved/edited later (see plugin.cpp's
        // stateSave/stateLoad). regionCount is UI-label-only (1 for a plain
        // sample). path is the original absolute path, kept for the Sample
        // tab's file-list display and for re-deriving sampleRelativePath/
        // recognizing the file on reload.
        struct StackItem {
            std::string path;
            bool isSfz = false;
            std::string sampleRelativePath; // !isSfz only
            std::string regionsText;        // isSfz only
            int regionCount = 1;
        };
        std::vector<StackItem> stack;

        std::string lastSfzText;
        // Meaningful only when stack.size()==1 && !stack[0].isSfz (a single
        // plain sample, no layering) - numFrames <= 0 and loopStart/
        // EndFrame < 0 otherwise, meaning "no sample"/"no loop points"
        // respectively. Drives the offset slider's max and the waveform
        // view (itself only shown in that same single-sample situation -
        // see drawSampleTab - since loop markers/an offset scrubber don't
        // mean much once multiple items are stacked).
        int64_t numFrames = 0;
        int64_t loopStartFrame = -1;
        int64_t loopEndFrame = -1;
        // Opcodes tab: raw user-typed text, written verbatim into the
        // generated SFZ after every other <global> opcode (see
        // SfzDocument.cpp's buildSfzText). Free-form, so it's a string, not
        // an atomic - GUI thread only, same convention as the stack above.
        std::string customOpcodesText = "//custom opcodes here\nlfo04_pitch_oncc1=40";

        // Last stack load/restore failure (see plugin.cpp's
        // loadStack/restoreStack) - shown near the waveform/file-list view.
        // Cleared on the next successful load, remove, or clear.
        std::string stackError;

        // Last Save/Load Preset or Profile failure (see plugin.cpp's
        // saveSoloSamplerPreset/loadSoloSamplerPreset and drawEditorUI's
        // preset button row) - a zenity cancel is NOT an error and leaves
        // this untouched. Cleared on the next successful save or load.
        std::string presetError;

        // Directory of the most recently loaded sample or multisample SFZ
        // (set in plugin.cpp's loadStack, so it tracks both drag-and-drop
        // drops and file-dialog picks). Used as the in-house FileDialog's
        // start directory the next time it's opened - see editor_ui.cpp.
        std::string lastBrowseDir;

        // Pitch tab's "Tuning" section: the currently loaded Scala (.scl)
        // file, picked via zenity (see plugin.cpp's handleLoadScalaFile) -
        // empty means standard 12-TET. Re-applied to the engine after every
        // SFZ regenerate (see regenerateAndLoadSfz), since tuning is a
        // synth-level setting a plain SFZ reload doesn't otherwise preserve.
        // scalaError is the last load failure, advisory only - a missing
        // file falls back to standard tuning (see plugin.cpp's
        // restoreScala) rather than leaving stale/undefined tuning active.
        // Same DAW-session-only persistence as SharedParams::tuningFrequency
        // - deliberately not part of PresetFields/.sspreset/.ssprofile.
        std::string scalaFilePath;
        std::string scalaError;
        // Directory of the most recently loaded scala file (set in
        // plugin.cpp's handleLoadScalaFile) - seeds zenity's starting
        // folder the next time "Load Scala File..." is clicked, same
        // "remember where the user's files actually are" convention as
        // lastBrowseDir above, kept separate since scala files usually live
        // somewhere else entirely from samples/SFZs. Runtime-only, not
        // persisted (same as lastBrowseDir) - resets on plugin reload.
        std::string scalaBrowseDir;
    } guiState;
};
