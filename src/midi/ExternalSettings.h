// Plain settings of the "External" tab's MIDI preprocessor (see
// ExternalMidi.h for what each one does). Kept separate from the processor
// itself so the preset/state formats can carry it without depending on
// sfizz.
#pragma once

#include <cstdint>
#include <type_traits>

inline constexpr const char* kExtPolarityNames[3] = {"Bipolar (+/-)", "Positive (+)",
                                                     "Negative (-)"};
inline constexpr const char* kExtSlopeModeNames[2] = {"Vital (power)", "ExMachina (cosine)"};
inline constexpr const char* kExtDriftModelNames[3] = {"Off", "ViolaExMachina",
                                                       "ChorusExMachina"};
inline constexpr const char* kExtVibratoModelNames[2] = {"ViolaExMachina", "ChorusExMachina"};

// Plain (non-atomic) snapshot of every External tab setting - the single
// place their defaults live. Only int32_t/float members, so it has no
// padding and is persisted as a raw, size-prefixed blob (see
// writeExternalBlob/readExternalBlob in PresetFile.cpp and plugin.cpp's
// stateSave/stateLoad): new fields must only ever be APPENDED, so an older
// (shorter) blob still loads with the newer fields left at their defaults.
struct ExternalSettings {
    int32_t enabled = 0;
    int32_t mono = 0; // mono legato (forces monophonic MIDI, not MPE), CC16

    // Pitch randomizer: note-on slide (random start offset -> 0) and
    // note-off slide (0 -> random offset during release).
    float noteOnCents = 0.0f;   // 0..1200
    int32_t noteOnPolarity = 0; // kExtPolarityNames
    float noteOnMinMs = 30.0f;  // 0..2000
    float noteOnMaxMs = 120.0f; // 0..2000
    float noteOffCents = 0.0f;
    int32_t noteOffPolarity = 0;
    float noteOffMinMs = 50.0f;
    float noteOffMaxMs = 200.0f;
    int32_t driftModel = 0;    // kExtDriftModelNames
    float driftScale = 100.0f; // 0..400 % of the model's own depth

    // Mono legato glide.
    float glideMs = 0.0f;       // 0..1500, 0 = off, CC14
    int32_t glideSlopeMode = 0; // kExtSlopeModeNames
    float glideSlope = 0.0f;    // -8..8 (Vital mode only)

    // Amplitude expression during a glide.
    int32_t ampExprEnabled = 0;   // CC15
    float ampExprAmount = 50.0f;  // 0..100 %
    int32_t ampExprSlopeMode = 1; // kExtSlopeModeNames
    float ampExprSlope = 0.0f;    // -8..8 (Vital mode only)

    // Vibrato (pitch-wheel based).
    int32_t vibratoAmount = 0;      // 0..127, CC1
    float vibratoDepthCents = 35.0f; // 0..200, depth at CC1=127
    float vibratoRateHz = 5.2f;      // 0.5..12
    int32_t vibratoModel = 0;        // kExtVibratoModelNames

    // Mono legato: retrigger the sample on every legato key (still mono -
    // the previous note is released first) instead of only bending the
    // first one. The glide/amp expression still run, starting from the
    // previous pitch.
    int32_t retrigger = 0;

    // Vibrato routing: vibratoPitch applies it to the pitch wheel (on by
    // default - the pre-existing behavior); vibratoCcOut (0 = off, 1..127)
    // also sends the same vibrato signal to sfizz as a high-resolution CC,
    // centered at 0.5 (64) and swinging +-0.5 at full depth - pair it with
    // e.g. cutoff_oncc<N>=1200 cutoff_curvecc<N>=1 (sfizz's bipolar curve)
    // so the resting value is neutral.
    int32_t vibratoPitch = 1;
    int32_t vibratoCcOut = 0;
};
static_assert(std::is_trivially_copyable_v<ExternalSettings>);
