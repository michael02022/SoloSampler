// "External" tab: a MIDI preprocessor that sits between the host's MIDI
// input (plus the on-screen piano) and sfizz. Everything it does is
// expressed as plain MIDI the SFZ format already understands - mostly a
// synthesized pitch-wheel stream (random note-on/note-off slides,
// ExMachina-style drift, legato glide, vibrato) plus a post-render gain
// curve (amplitude expression during a glide). None of it is an SFZ
// opcode, so nothing here triggers regenerateAndLoadSfz.
//
// Every cents value is converted to pitch-wheel units through the
// instrument's own bendup=/benddown= (SharedParams::bendUpCents/
// bendDownCents) - so a legato glide or vibrato can never reach further
// than the bend range, and the bend range is what sets its resolution.
//
// Models referenced (see their sources for the originals):
// - Vital (mtytel/vital): portamento slope = futils::powerScale, -8..8.
// - ViolaExMachina / ChorusExMachina (peastman): cosine transitions
//   (0.5-0.5cos(pi t)), cubed-sine vibrato with drifting rate/depth, and a
//   slow Ornstein-Uhlenbeck pitch drift.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "midi/ExternalSettings.h"
#include "sfizz/SfizzEngine.h"

// Atomic mirror of ExternalSettings living in SharedParams. The GUI writes
// it from the sliders; the audio thread reads it every block and also
// WRITES the CC-followed fields (vibratoAmount<-CC1, glideMs<-CC14,
// ampExprEnabled<-CC15, mono<-CC16) when that CC arrives, so the sliders
// visibly follow the DAW - the slider value is simply what's used until a
// CC message overrides it.
struct ExternalParams {
    std::atomic<bool> enabled, mono;
    std::atomic<float> noteOnCents;
    std::atomic<int> noteOnPolarity;
    std::atomic<float> noteOnMinMs, noteOnMaxMs;
    std::atomic<float> noteOffCents;
    std::atomic<int> noteOffPolarity;
    std::atomic<float> noteOffMinMs, noteOffMaxMs;
    std::atomic<int> driftModel;
    std::atomic<float> driftScale;
    std::atomic<float> glideMs;
    std::atomic<int> glideSlopeMode;
    std::atomic<float> glideSlope;
    std::atomic<bool> ampExprEnabled;
    std::atomic<float> ampExprAmount;
    std::atomic<int> ampExprSlopeMode;
    std::atomic<float> ampExprSlope;
    std::atomic<int> vibratoAmount;
    std::atomic<float> vibratoDepthCents, vibratoRateHz;
    std::atomic<int> vibratoModel;
    std::atomic<bool> retrigger;
    std::atomic<bool> vibratoPitch;
    std::atomic<int> vibratoCcOut;

    ExternalParams() { store(ExternalSettings{}); }
    ExternalSettings load() const;
    void store(const ExternalSettings& s);
};

class ExternalMidiProcessor {
public:
    using MidiEvent = SfizzEngine::MidiEvent;

    ExternalMidiProcessor();

    // Non-audio thread (plugActivate).
    void prepare(double sampleRate, int maxFrames);

    // Audio thread. `in` must be ordered by delaySamples. Writes the events
    // sfizz should actually receive into `out` (ordered by delaySamples,
    // at most maxOut). When disabled this is a plain copy. engineMpe is
    // SharedParams::mpeEnabled (see MPE routing below). Returns true
    // when gain() holds a non-unity amplitude curve for this block that
    // the caller must multiply into the rendered output.
    bool process(const MidiEvent* in, int numIn, int numFrames, ExternalParams& params,
                 int bendUpCents, int bendDownCents, bool engineMpe, MidiEvent* out,
                 int& numOut, int maxOut);

    const float* gain() const { return gain_.data(); }

private:
    // ExMachina's quick LCG ("even quicker generator", Numerical Recipes).
    uint32_t rngState_ = 0;
    bool hasSpareNormal_ = false;
    float spareNormal_ = 0.0f;
    float uniform();
    float normal();
    float randomOffset(float cents, int polarity);
    float randomLengthSamples(float minMs, float maxMs);

    void resetVoiceState();
    void advanceTo(int t);
    void emitBend(int t);
    void updateModulators(int dtSamples);
    void push(const MidiEvent& e);
    void startFreshNote(int note, int velocity, int channel, int t);
    void startGlide(int targetNote, int t);
    void startNoteOffSlide();
    // includePolyHeld: also note-off every held key in poly mode (mono
    // toggle) - not on disable, where those keys' own note-offs still pass
    // straight through later.
    void releaseAll(int t, bool includePolyHeld);
    bool removeHeld(int note);
    void addHeld(int note);
    float glideCents() const;
    float onSlideCents() const;
    float offSlideCents() const;
    float wheelToCents(int wheel) const;
    int centsToWheel(float cents) const;
    float ampGain() const;
    float totalCents() const;
    float vibratoCents() const;
    void emitVibratoCc(int t);

    double sr_ = 48000.0;
    std::vector<float> gain_;

    // Per-block context (set at the top of process()).
    ExternalSettings s_{};
    int bendUp_ = 2400, bendDown_ = -2400;
    MidiEvent* out_ = nullptr;
    int numOut_ = 0, maxOut_ = 0;
    int cursor_ = 0, nextEmit_ = 0, emitInterval_ = 32;
    bool gainUsed_ = false;

    bool wasEnabled_ = false;
    bool monoApplied_ = false;
    // Mono + engine MPE: every note sfizz receives (fresh or retriggered)
    // goes out on the next MPE member channel (2-16, round robin - channel
    // 1 is MPE's master, whose bend would move every voice). Each note then
    // owns its pitch bend: once a new note takes over, the previous one's
    // release tail keeps the pitch it was left at instead of being dragged
    // along by the new glide, so the release blends into the next note.
    bool mpeApplied_ = false;
    int nextMemberChannel_ = 1;
    int takeMemberChannel();
    int userWheel_ = 0; // last pitch wheel received from the host (-8192..8191)
    int lastWheel_ = 0; // last pitch wheel sent to sfizz (kNoWheel = unknown)
    int channel_ = 0;

    // Keys currently held (oldest first, last-note priority).
    int held_[128]{};
    int heldCount_ = 0;
    // Mono: the one note sfizz is actually playing (-1 = none). Every
    // other held key is reached by bending this one - never retriggered.
    int baseNote_ = -1;
    int heldVelocity_[128]{}; // by note number, for retriggering a held key
    int currentTarget_ = -1; // held key the glide is heading to
    // Releasing the glide target while other keys are held returns to the
    // last held key - deferred until every event at that same sample is
    // in, so releasing several keys at once doesn't retrigger (and
    // instantly release) each key in between.
    bool pendingReturn_ = false;
    int pendingReturnT_ = 0;
    void resolvePendingReturn();

    // Legato glide (cents relative to baseNote_).
    float glideFrom_ = 0.0f, glideTo_ = 0.0f;
    int64_t glideLen_ = 0, glidePos_ = 0;

    // Note-on slide: offset -> 0. Note-off slide: 0 -> offset.
    float onSlideFrom_ = 0.0f;
    int64_t onSlideLen_ = 0, onSlidePos_ = 0;
    float offSlideTo_ = 0.0f;
    int64_t offSlideLen_ = 0, offSlidePos_ = 0;
    bool offSlideActive_ = false;

    // Amplitude expression: a triangle over the glide's own length - down
    // to the floor over the first half, back up to 1 over the second half
    // (the mirror of the way down), each half shaped by the amp slope.
    bool ampActive_ = false;
    float ampStart_ = 1.0f, ampFloor_ = 1.0f;
    int64_t ampDown_ = 0, ampUp_ = 0, ampPos_ = 0;

    // ExMachina drift (Ornstein-Uhlenbeck-like state, unit variance).
    float drift_ = 0.0f;
    double driftTimer_ = 0.0;

    // Vibrato.
    double vibPhase_ = 0.0; // 0..4, like ExMachina (cos(0.5*pi*phase) rate drift)
    double vibInc_ = 0.0;
    float vibAmpDrift_ = 0.0f;
    double vibDriftTimer_ = 0.0;
    float vibDepth_ = 0.0f; // smoothed cents
    int lastVibCcNumber_ = 0;   // CC the vibrato was last sent on (0 = none)
    float lastVibCcValue_ = -1.0f;
};
