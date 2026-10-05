#include "ExternalMidi.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <random>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kNoWheel = INT_MIN;

// ExMachina transitions: weight2 = 0.5-0.5cos(pi*fraction).
float cosineCurve(float t) {
    return 0.5f - 0.5f * static_cast<float>(std::cos(kPi * t));
}

// Vital's futils::powerScale (portamento_slope, -8..8).
float powerScale(float t, float power) {
    if (std::fabs(power) < 0.01f) return t;
    return (std::exp(power * t) - 1.0f) / (std::exp(power) - 1.0f);
}

float shapeCurve(float t, int mode, float slope) {
    t = std::clamp(t, 0.0f, 1.0f);
    return mode == 1 ? cosineCurve(t) : powerScale(t, slope);
}

// Relative frequency amplitudes from the ExMachina sources, as cents:
// ViolaExMachina frequency_drift_amplitude=0.002, ChorusExMachina 0.005.
constexpr float kViolaDriftCents = 3.4617f;  // 1200*log2(1.002)
constexpr float kChorusDriftCents = 8.6376f; // 1200*log2(1.005)

} // namespace

ExternalSettings ExternalParams::load() const {
    ExternalSettings s;
    s.enabled = enabled.load() ? 1 : 0;
    s.mono = mono.load() ? 1 : 0;
    s.noteOnCents = noteOnCents.load();
    s.noteOnPolarity = noteOnPolarity.load();
    s.noteOnMinMs = noteOnMinMs.load();
    s.noteOnMaxMs = noteOnMaxMs.load();
    s.noteOffCents = noteOffCents.load();
    s.noteOffPolarity = noteOffPolarity.load();
    s.noteOffMinMs = noteOffMinMs.load();
    s.noteOffMaxMs = noteOffMaxMs.load();
    s.driftModel = driftModel.load();
    s.driftScale = driftScale.load();
    s.glideMs = glideMs.load();
    s.glideSlopeMode = glideSlopeMode.load();
    s.glideSlope = glideSlope.load();
    s.ampExprEnabled = ampExprEnabled.load() ? 1 : 0;
    s.ampExprAmount = ampExprAmount.load();
    s.ampExprSlopeMode = ampExprSlopeMode.load();
    s.ampExprSlope = ampExprSlope.load();
    s.vibratoAmount = vibratoAmount.load();
    s.vibratoDepthCents = vibratoDepthCents.load();
    s.vibratoRateHz = vibratoRateHz.load();
    s.vibratoModel = vibratoModel.load();
    s.retrigger = retrigger.load() ? 1 : 0;
    s.vibratoPitch = vibratoPitch.load() ? 1 : 0;
    s.vibratoCcOut = vibratoCcOut.load();
    return s;
}

void ExternalParams::store(const ExternalSettings& s) {
    enabled = s.enabled != 0;
    mono = s.mono != 0;
    noteOnCents = s.noteOnCents;
    noteOnPolarity = std::clamp<int>(s.noteOnPolarity, 0, 2);
    noteOnMinMs = s.noteOnMinMs;
    noteOnMaxMs = s.noteOnMaxMs;
    noteOffCents = s.noteOffCents;
    noteOffPolarity = std::clamp<int>(s.noteOffPolarity, 0, 2);
    noteOffMinMs = s.noteOffMinMs;
    noteOffMaxMs = s.noteOffMaxMs;
    driftModel = std::clamp<int>(s.driftModel, 0, 2);
    driftScale = s.driftScale;
    glideMs = s.glideMs;
    glideSlopeMode = std::clamp<int>(s.glideSlopeMode, 0, 1);
    glideSlope = s.glideSlope;
    ampExprEnabled = s.ampExprEnabled != 0;
    ampExprAmount = s.ampExprAmount;
    ampExprSlopeMode = std::clamp<int>(s.ampExprSlopeMode, 0, 1);
    ampExprSlope = s.ampExprSlope;
    vibratoAmount = std::clamp<int>(s.vibratoAmount, 0, 127);
    vibratoDepthCents = s.vibratoDepthCents;
    vibratoRateHz = s.vibratoRateHz;
    vibratoModel = std::clamp<int>(s.vibratoModel, 0, 1);
    retrigger = s.retrigger != 0;
    vibratoPitch = s.vibratoPitch != 0;
    vibratoCcOut = std::clamp<int>(s.vibratoCcOut, 0, 127);
}

ExternalMidiProcessor::ExternalMidiProcessor() {
    std::random_device rd;
    rngState_ = rd();
    drift_ = normal();
    vibAmpDrift_ = normal();
    resetVoiceState();
}

void ExternalMidiProcessor::prepare(double sampleRate, int maxFrames) {
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    gain_.assign(static_cast<size_t>(std::max(maxFrames, 1)), 1.0f);
}

float ExternalMidiProcessor::uniform() {
    rngState_ = rngState_ * 1664525u + 1013904223u;
    return static_cast<float>(rngState_) * (1.0f / 4294967296.0f);
}

float ExternalMidiProcessor::normal() {
    if (hasSpareNormal_) {
        hasSpareNormal_ = false;
        return spareNormal_;
    }
    for (;;) {
        const float x = 2.0f * uniform() - 1.0f;
        const float y = 2.0f * uniform() - 1.0f;
        const float r2 = x * x + y * y;
        if (r2 < 1.0f && r2 != 0.0f) {
            const float m = std::sqrt(-2.0f * std::log(r2) / r2);
            spareNormal_ = y * m;
            hasSpareNormal_ = true;
            return x * m;
        }
    }
}

float ExternalMidiProcessor::randomOffset(float cents, int polarity) {
    if (cents <= 0.0f) return 0.0f;
    const float u = uniform();
    switch (polarity) {
        case 1: return u * cents;
        case 2: return -u * cents;
        default: return (2.0f * u - 1.0f) * cents;
    }
}

float ExternalMidiProcessor::randomLengthSamples(float minMs, float maxMs) {
    const float lo = std::max(0.0f, std::min(minMs, maxMs));
    const float hi = std::max(0.0f, std::max(minMs, maxMs));
    return (lo + (hi - lo) * uniform()) * static_cast<float>(sr_) / 1000.0f;
}

void ExternalMidiProcessor::resetVoiceState() {
    heldCount_ = 0;
    baseNote_ = -1;
    currentTarget_ = -1;
    pendingReturn_ = false;
    glideFrom_ = glideTo_ = 0.0f;
    glideLen_ = glidePos_ = 0;
    onSlideFrom_ = 0.0f;
    onSlideLen_ = onSlidePos_ = 0;
    offSlideTo_ = 0.0f;
    offSlideLen_ = offSlidePos_ = 0;
    offSlideActive_ = false;
    ampActive_ = false;
    lastWheel_ = kNoWheel;
}

float ExternalMidiProcessor::wheelToCents(int wheel) const {
    if (wheel >= 0) return wheel / 8191.0f * static_cast<float>(std::abs(bendUp_));
    return wheel / 8192.0f * static_cast<float>(std::abs(bendDown_));
}

int ExternalMidiProcessor::centsToWheel(float cents) const {
    double v;
    if (cents >= 0.0f)
        v = cents / std::max(1, std::abs(bendUp_)) * 8191.0;
    else
        v = cents / std::max(1, std::abs(bendDown_)) * 8192.0;
    return static_cast<int>(std::clamp<long>(std::lround(v), -8192, 8191));
}

float ExternalMidiProcessor::glideCents() const {
    if (glideLen_ <= 0 || glidePos_ >= glideLen_) return glideTo_;
    const float t = static_cast<float>(glidePos_) / static_cast<float>(glideLen_);
    return glideFrom_ + (glideTo_ - glideFrom_) * shapeCurve(t, s_.glideSlopeMode, s_.glideSlope);
}

float ExternalMidiProcessor::onSlideCents() const {
    if (onSlideLen_ <= 0 || onSlidePos_ >= onSlideLen_) return 0.0f;
    const float t = static_cast<float>(onSlidePos_) / static_cast<float>(onSlideLen_);
    return onSlideFrom_ * (1.0f - cosineCurve(t));
}

float ExternalMidiProcessor::offSlideCents() const {
    if (!offSlideActive_) return 0.0f;
    if (offSlideLen_ <= 0 || offSlidePos_ >= offSlideLen_) return offSlideTo_;
    const float t = static_cast<float>(offSlidePos_) / static_cast<float>(offSlideLen_);
    return offSlideTo_ * cosineCurve(t);
}

float ExternalMidiProcessor::ampGain() const {
    if (!ampActive_) return 1.0f;
    const int mode = s_.ampExprSlopeMode;
    const float slope = s_.ampExprSlope;
    if (ampPos_ < ampDown_) {
        const float t = static_cast<float>(ampPos_) / static_cast<float>(ampDown_);
        return ampStart_ + (ampFloor_ - ampStart_) * shapeCurve(t, mode, slope);
    }
    if (ampUp_ <= 0) return 1.0f;
    const float t = static_cast<float>(ampPos_ - ampDown_) / static_cast<float>(ampUp_);
    if (t >= 1.0f) return 1.0f;
    // Mirror image of the way down, so the triangle is symmetric.
    return 1.0f + (ampFloor_ - 1.0f) * shapeCurve(1.0f - t, mode, slope);
}

float ExternalMidiProcessor::totalCents() const {
    float cents = wheelToCents(userWheel_) + glideCents() + onSlideCents() + offSlideCents();

    if (s_.driftModel == 1 || s_.driftModel == 2) {
        const float depth = s_.driftModel == 1 ? kViolaDriftCents : kChorusDriftCents;
        cents += depth * (s_.driftScale / 100.0f) * drift_;
    }

    if (s_.vibratoPitch) cents += vibratoCents();
    return cents;
}

// Cubed sine, depth modulated by a slowly drifting factor (ExMachina's
// vibrato_amplitude_drift_amplitude = 0.4).
float ExternalMidiProcessor::vibratoCents() const {
    const float v = static_cast<float>(std::sin(2.0 * kPi * vibPhase_));
    return vibDepth_ * std::max(0.0f, 1.0f + 0.4f * vibAmpDrift_) * v * v * v;
}

void ExternalMidiProcessor::emitVibratoCc(int t) {
    const int number = std::clamp(s_.vibratoCcOut, 0, 127);
    MidiEvent e{};
    e.type = MidiEvent::Type::CC;
    e.delaySamples = t;
    e.channel = channel_;
    if (number != lastVibCcNumber_) {
        // Switched CCs: park the old one back at its neutral center.
        if (lastVibCcNumber_ > 0) {
            e.ccNumber = lastVibCcNumber_;
            e.ccValue = 64;
            e.ccValueHd = 0.5f;
            push(e);
        }
        lastVibCcNumber_ = number;
        lastVibCcValue_ = -1.0f;
    }
    if (number <= 0) return;
    // Normalized against the max depth, so the swing follows CC1 the same
    // way the pitch vibrato does.
    const float maxDepth = std::max(0.01f, s_.vibratoDepthCents);
    const float norm = 0.5f + 0.5f * std::clamp(vibratoCents() / maxDepth, -1.0f, 1.0f);
    if (std::fabs(norm - lastVibCcValue_) < 1.0e-4f) return;
    lastVibCcValue_ = norm;
    e.ccNumber = number;
    e.ccValue = static_cast<int>(std::lround(norm * 127.0f));
    e.ccValueHd = norm;
    push(e);
}

void ExternalMidiProcessor::updateModulators(int dtSamples) {
    const double dt = static_cast<double>(dtSamples);

    // Vibrato depth follows CC1 with a ~30 ms smoothing so stepped CC
    // values don't zipper.
    const float target =
        s_.vibratoDepthCents * static_cast<float>(std::clamp(s_.vibratoAmount, 0, 127)) / 127.0f;
    const float a = 1.0f - static_cast<float>(std::exp(-dt / (0.03 * sr_)));
    vibDepth_ += (target - vibDepth_) * a;

    // Rate drift: Viola 0.1, Chorus 0.05 (vibrato_frequency_drift_amplitude),
    // cos(0.5*pi*phase) with phase wrapping at 4 cycles, as in both sources.
    const bool viola = s_.vibratoModel == 0;
    const double fd = viola ? 0.1 : 0.05;
    const double rate = std::max(0.0f, s_.vibratoRateHz);
    vibInc_ = rate * (1.0 + fd * std::cos(0.5 * kPi * vibPhase_)) / sr_;

    // Depth drift update cadence: Viola updates once per period of the
    // sounding note, Chorus every 1000 samples at 48 kHz.
    double interval;
    if (viola) {
        const float note = baseNote_ >= 0 ? baseNote_ + glideCents() / 100.0f : 60.0f;
        const double freq = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
        interval = std::max(1.0, sr_ / std::max(1.0, freq));
    } else {
        interval = 1000.0 / 48000.0 * sr_;
    }
    vibDriftTimer_ += dt;
    while (vibDriftTimer_ >= interval) {
        vibDriftTimer_ -= interval;
        vibAmpDrift_ = 0.99f * vibAmpDrift_ + 0.1f * normal();
    }

    // Pitch drift: Viola is a continuous OU process with a 1 s time
    // constant (decay = exp(-dt)), Chorus the 0.99/0.1 update every 1000
    // samples at 48 kHz.
    if (s_.driftModel == 1) {
        const double decay = std::exp(-dt / sr_);
        drift_ = static_cast<float>(decay * drift_ +
                                    std::sqrt(1.0 - decay * decay) * normal());
    } else if (s_.driftModel == 2) {
        const double chorusInterval = 1000.0 / 48000.0 * sr_;
        driftTimer_ += dt;
        while (driftTimer_ >= chorusInterval) {
            driftTimer_ -= chorusInterval;
            drift_ = 0.99f * drift_ + 0.1f * normal();
        }
    }
}

void ExternalMidiProcessor::push(const MidiEvent& e) {
    if (numOut_ < maxOut_) out_[numOut_++] = e;
}

void ExternalMidiProcessor::emitBend(int t) {
    const int wheel = centsToWheel(totalCents());
    if (wheel == lastWheel_) return;
    lastWheel_ = wheel;
    MidiEvent e{};
    e.type = MidiEvent::Type::PitchWheel;
    e.delaySamples = t;
    e.channel = channel_;
    e.pitch = wheel;
    push(e);
}

void ExternalMidiProcessor::advanceTo(int t) {
    const int gainSize = static_cast<int>(gain_.size());
    for (int i = cursor_; i < t; ++i) {
        if (i >= nextEmit_) {
            updateModulators(emitInterval_);
            emitBend(i);
            emitVibratoCc(i);
            nextEmit_ += emitInterval_;
        }
        if (glidePos_ < glideLen_) ++glidePos_;
        if (onSlidePos_ < onSlideLen_) ++onSlidePos_;
        if (offSlideActive_ && offSlidePos_ < offSlideLen_) ++offSlidePos_;
        vibPhase_ += vibInc_;
        if (vibPhase_ >= 4.0) vibPhase_ -= 4.0;

        float g = 1.0f;
        if (ampActive_) {
            g = ampGain();
            ++ampPos_;
            if (ampPos_ >= ampDown_ + ampUp_) ampActive_ = false;
        }
        if (i < gainSize) {
            gain_[i] = g;
            if (g != 1.0f) gainUsed_ = true;
        }
    }
    if (t > cursor_) cursor_ = t;
}

bool ExternalMidiProcessor::removeHeld(int note) {
    for (int i = 0; i < heldCount_; ++i) {
        if (held_[i] == note) {
            for (int j = i; j + 1 < heldCount_; ++j) held_[j] = held_[j + 1];
            --heldCount_;
            return true;
        }
    }
    return false;
}

void ExternalMidiProcessor::addHeld(int note) {
    if (heldCount_ < 128) held_[heldCount_++] = note;
}

void ExternalMidiProcessor::startFreshNote(int note, int velocity, int channel, int t) {
    baseNote_ = note;
    currentTarget_ = note;
    glideFrom_ = glideTo_ = 0.0f;
    glideLen_ = glidePos_ = 0;
    ampActive_ = false;
    offSlideActive_ = false;
    onSlideFrom_ = randomOffset(s_.noteOnCents, s_.noteOnPolarity);
    onSlideLen_ = static_cast<int64_t>(randomLengthSamples(s_.noteOnMinMs, s_.noteOnMaxMs));
    onSlidePos_ = 0;
    // Bend first, so the note starts already at its slide's start offset.
    emitBend(t);
    MidiEvent e{};
    e.type = MidiEvent::Type::NoteOn;
    e.delaySamples = t;
    e.channel = channel;
    e.noteNumber = note;
    e.velocity = velocity;
    push(e);
}

void ExternalMidiProcessor::resolvePendingReturn() {
    if (!pendingReturn_) return;
    pendingReturn_ = false;
    if (baseNote_ >= 0 && heldCount_ > 0 && held_[heldCount_ - 1] != currentTarget_)
        startGlide(held_[heldCount_ - 1], pendingReturnT_); // back to the last held key
}

int ExternalMidiProcessor::takeMemberChannel() {
    const int c = nextMemberChannel_;
    nextMemberChannel_ = nextMemberChannel_ >= 15 ? 1 : nextMemberChannel_ + 1;
    return c;
}

void ExternalMidiProcessor::startGlide(int targetNote, int t) {
    currentTarget_ = targetNote;
    float from = glideCents();
    const bool retrigger = s_.retrigger != 0;
    if (retrigger) {
        // Still monophonic: release the sounding note, then re-base the
        // glide on the new one so it starts from the previous pitch.
        MidiEvent off{};
        off.type = MidiEvent::Type::NoteOff;
        off.delaySamples = t;
        off.channel = channel_;
        off.noteNumber = baseNote_;
        push(off);
        from += static_cast<float>(baseNote_ - targetNote) * 100.0f;
        baseNote_ = targetNote;
        if (mpeApplied_) {
            // New note, new channel: the old channel's bend is left as is.
            channel_ = takeMemberChannel();
            lastWheel_ = kNoWheel;
        }
    }
    const float target = static_cast<float>(targetNote - baseNote_) * 100.0f;

    if (s_.glideMs <= 0.0f) {
        // Glide off: jump straight to the new pitch.
        glideFrom_ = glideTo_ = target;
        glideLen_ = glidePos_ = 0;
    } else {
        glideFrom_ = from;
        glideTo_ = target;
        glideLen_ = std::max<int64_t>(1, static_cast<int64_t>(s_.glideMs * sr_ / 1000.0));
        glidePos_ = 0;
    }

    if (retrigger) {
        emitBend(t); // the new note starts at the glide's start pitch
        MidiEvent on{};
        on.type = MidiEvent::Type::NoteOn;
        on.delaySamples = t;
        on.channel = channel_;
        on.noteNumber = targetNote;
        on.velocity = heldVelocity_[targetNote & 127];
        push(on);
    }

    if (glideLen_ > 0 && s_.ampExprEnabled && s_.ampExprAmount > 0.0f) {
        ampStart_ = ampGain();
        ampFloor_ = 1.0f - std::clamp(s_.ampExprAmount, 0.0f, 100.0f) / 100.0f;
        // Triangle spanning the pitch glide: deepest at its midpoint.
        ampDown_ = std::max<int64_t>(1, glideLen_ / 2);
        ampUp_ = std::max<int64_t>(1, glideLen_ - ampDown_);
        ampPos_ = 0;
        ampActive_ = true;
    }
}

void ExternalMidiProcessor::startNoteOffSlide() {
    offSlideTo_ = randomOffset(s_.noteOffCents, s_.noteOffPolarity);
    offSlideLen_ = static_cast<int64_t>(randomLengthSamples(s_.noteOffMinMs, s_.noteOffMaxMs));
    offSlidePos_ = 0;
    offSlideActive_ = offSlideTo_ != 0.0f;
}

void ExternalMidiProcessor::releaseAll(int t, bool includePolyHeld) {
    MidiEvent e{};
    e.type = MidiEvent::Type::NoteOff;
    e.delaySamples = t;
    e.channel = channel_;
    if (monoApplied_) {
        if (baseNote_ >= 0) {
            e.noteNumber = baseNote_;
            push(e);
        }
    } else if (includePolyHeld) {
        for (int i = 0; i < heldCount_; ++i) {
            e.noteNumber = held_[i];
            push(e);
        }
    }
    resetVoiceState();
    emitBend(t);
}

bool ExternalMidiProcessor::process(const MidiEvent* in, int numIn, int numFrames,
                                    ExternalParams& params, int bendUpCents,
                                    int bendDownCents, bool engineMpe, MidiEvent* out,
                                    int& numOut, int maxOut) {
    s_ = params.load();
    bendUp_ = bendUpCents;
    bendDown_ = bendDownCents;
    out_ = out;
    numOut_ = 0;
    maxOut_ = maxOut;
    gainUsed_ = false;

    if (!s_.enabled) {
        if (wasEnabled_) {
            // Leaving: stop the mono note (its key may already be up), and
            // hand the wheel back to whatever the host last sent.
            releaseAll(0, /*includePolyHeld=*/false);
            MidiEvent e{};
            e.type = MidiEvent::Type::PitchWheel;
            e.channel = channel_;
            e.pitch = userWheel_;
            push(e);
            wasEnabled_ = false;
        }
        for (int i = 0; i < numIn; ++i) {
            if (in[i].type == MidiEvent::Type::PitchWheel) userWheel_ = in[i].pitch;
            push(in[i]);
        }
        numOut = numOut_;
        return false;
    }

    if (!wasEnabled_) {
        resetVoiceState();
        monoApplied_ = s_.mono != 0;
        mpeApplied_ = engineMpe;
        wasEnabled_ = true;
    }
    if ((s_.mono != 0) != monoApplied_ || engineMpe != mpeApplied_) {
        releaseAll(0, /*includePolyHeld=*/true);
        monoApplied_ = s_.mono != 0;
        mpeApplied_ = engineMpe;
    }

    // At most ~64 generated bends per block; sfizz interpolates linearly
    // between consecutive pitch-wheel events, so this stays smooth.
    emitInterval_ = std::max(32, (numFrames + 63) / 64);
    cursor_ = 0;
    nextEmit_ = 0;

    for (int i = 0; i < numIn; ++i) {
        const MidiEvent& e = in[i];
        const int t = std::clamp(e.delaySamples, 0, std::max(0, numFrames - 1));
        if (pendingReturn_ && t != pendingReturnT_) resolvePendingReturn();
        advanceTo(t);

        switch (e.type) {
            case MidiEvent::Type::NoteOn:
                pendingReturn_ = false; // the new key becomes the target instead
                // Mono: the bend must follow the one sounding note, so only
                // a fresh note picks the channel - its incoming one, or the
                // next MPE member channel when engine MPE is on.
                if (!monoApplied_) {
                    channel_ = e.channel;
                } else if (baseNote_ < 0) {
                    if (mpeApplied_) {
                        channel_ = takeMemberChannel();
                        lastWheel_ = kNoWheel;
                    } else {
                        channel_ = e.channel;
                    }
                }
                removeHeld(e.noteNumber);
                addHeld(e.noteNumber);
                heldVelocity_[e.noteNumber & 127] = e.velocity;
                if (monoApplied_) {
                    if (baseNote_ < 0)
                        startFreshNote(e.noteNumber, e.velocity, channel_, t);
                    else
                        startGlide(e.noteNumber, t); // reaches sfizz only with retrigger
                } else {
                    offSlideActive_ = false;
                    onSlideFrom_ = randomOffset(s_.noteOnCents, s_.noteOnPolarity);
                    onSlideLen_ =
                        static_cast<int64_t>(randomLengthSamples(s_.noteOnMinMs, s_.noteOnMaxMs));
                    onSlidePos_ = 0;
                    emitBend(t);
                    push(e);
                }
                break;

            case MidiEvent::Type::NoteOff: {
                const bool wasHeld = removeHeld(e.noteNumber);
                if (!monoApplied_) {
                    push(e);
                    if (heldCount_ == 0) startNoteOffSlide();
                    break;
                }
                // Not one of ours (sounding from before mono/enable): let
                // sfizz release it so it can't hang.
                if (!wasHeld) {
                    push(e);
                    break;
                }
                if (heldCount_ == 0) {
                    if (baseNote_ >= 0) {
                        MidiEvent off = e;
                        off.noteNumber = baseNote_;
                        off.channel = channel_;
                        push(off);
                        startNoteOffSlide();
                        baseNote_ = -1;
                        currentTarget_ = -1;
                    }
                } else if (e.noteNumber == currentTarget_) {
                    pendingReturn_ = true;
                    pendingReturnT_ = t;
                }
                break;
            }

            case MidiEvent::Type::CC: {
                push(e);
                const int v = e.ccValue;
                if (e.ccNumber == 1) {
                    s_.vibratoAmount = v;
                    params.vibratoAmount = v;
                } else if (e.ccNumber == 14) {
                    s_.glideMs = v / 127.0f * 1500.0f;
                    params.glideMs = s_.glideMs;
                } else if (e.ccNumber == 15) {
                    s_.ampExprEnabled = v >= 64 ? 1 : 0;
                    params.ampExprEnabled = v >= 64;
                } else if (e.ccNumber == 16) {
                    const bool on = v >= 64;
                    s_.mono = on ? 1 : 0;
                    params.mono = on;
                    if (on != monoApplied_) {
                        releaseAll(t, /*includePolyHeld=*/true);
                        monoApplied_ = on;
                    }
                }
                break;
            }

            case MidiEvent::Type::PitchWheel:
                // Folded into every generated bend instead of passed through.
                userWheel_ = e.pitch;
                break;
        }
    }
    resolvePendingReturn();
    advanceTo(numFrames);

    numOut = numOut_;
    return gainUsed_ && numFrames <= static_cast<int>(gain_.size());
}
