#include "SfzDocument.h"

#include <cmath>
#include <iomanip>
#include <sstream>

std::string buildSfzText(const SfzRegionParams& params) {
    std::ostringstream sfz;

    // <control>: defaults every CC the rest of this file exposes an _oncc
    // opcode on to 127 (full), so with no external CC ever received the
    // _oncc<N>=V opcode behaves exactly like a plain static V= opcode -
    // that's what lets tune/offset/pan/the FX tab's detune/delay/phase be
    // both a normal slider AND live MIDI-CC-controllable on the same
    // opcode. See ExplicacionPlugin.txt's CC135/136/137 convention (Pan/
    // Amp/Filter Random) for the same trick used one level up.
    sfz << "<control>\n"
        << "set_cc89=127 // FX delay\n"
        << "set_cc90=127 // FX tune\n"
        << "set_cc116=127 // FX phase\n"
        << "set_cc117=127 // volume (also drives Opcode FX's Unison pan_oncc117)\n"
        << "set_cc118=127 // offset\n"
        << "set_cc119=127 // tune\n"
        << "note_offset=" << params.character << "\n";

    // Real FX: sfizz's built-in fverb reverb. Its <effect> header must sit
    // between <control> and <global> (per user spec) - unlike the "Opcode
    // FX" chorus/unison effect further down, which is opcode-emulated via
    // <group>/<region> repetition rather than a real DSP effect block.
    // Gated behind reverbEnabled (default off) - when off, nothing is
    // written between <control> and <global>, same as before this feature
    // existed.
    if (params.reverbEnabled) {
        sfz << "<effect> type=fverb\n"
            << "reverb_type=" << params.reverbType << "\n\n"
            << std::fixed << std::setprecision(5)
            << "reverb_input=" << params.reverbInput << "\n"
            << "reverb_predelay=" << params.reverbPredelay << "\n"
            << "reverb_size=" << params.reverbSize << "\n"
            << "reverb_tone=" << params.reverbTone << "\n"
            << "reverb_damp=" << params.reverbDamp << "\n\n"
            << "reverb_dry=" << params.reverbDry << "\n"
            << "reverb_wet=" << params.reverbWet << "\n\n";
    }

    sfz << "<global>\n"
        << "bendup=" << params.bendUpCents << "\n"
        << "benddown=" << params.bendDownCents << "\n"
        << "sample_quality=" << params.quality << "\n";
    // loopMode=="none" is a sentinel (see kLoopModes in shared.hpp), not a
    // real SFZ value - skip the opcode entirely so sfizz decides for itself
    // (loop if the sample has embedded loop points, one-shot otherwise)
    // instead of forcing a specific mode.
    if (params.loopMode != "none")
        sfz << "loopmode=" << params.loopMode << "\n";
    // Piano right-click's multisample-mode nudge (see multisampleRootNote's
    // own comment) - added unconditionally, since it's a true no-op at its
    // default value of 60.
    const int transposeValue = params.character + (params.multisampleRootNote - 60);
    sfz << "polyphony=" << params.polyphony << "\n"
        << "note_polyphony=" << params.notePolyphony << "\n"
        << "tune_oncc119=" << static_cast<int>(std::lround(params.tuneCents)) << "\n"
        << "pitch_keycenter=" << params.rootNote << "\n"
        << "transpose=" << transposeValue << "\n"
        // cc117 is reused as the volume modulation source (see <control>'s
        // set_cc117=127 above) instead of pan - no pan_curvecc117= is
        // declared any more either, see the Pan section below.
        << "volume_oncc117=" << params.volume << "\n";

    if (params.offsetEnabled)
        sfz << "offset_oncc118=" << params.offsetValue << "\n";
    if (params.randomOffsetEnabled)
        sfz << "offset_oncc135=" << params.randomOffsetValue << "\n";

    // Pan: pan_oncc136 normally carries the Pan Random value; panAlternate
    // REPLACES it with pan_oncc137 instead (mutually exclusive, never
    // both). Then the always-on Pan LFO (lfo01_* - LFO numbering across
    // the whole plugin is Pan=01/Amp=02/Filter=03/Pitch=04).
    // pan_curvecc<N>=1 is required alongside pan_oncc<N>= for the CC->pan
    // modulation to behave as expected (per SFZ spec) - except cc117, see
    // below.
    //
    // "x2" (panX2): pan_oncc10=200 doubles the pan range for a plain CC10
    // MIDI pan message, and panMul below doubles every OTHER pan-opcode
    // VALUE this file writes (Pan Random here, Pan LFO just below, and
    // Opcode FX's pan_oncc117=/hard pan= further down) - for instruments
    // built from hard-panned mono samples (one mic per side) where the
    // normal pan range can't bring a channel back across center.
    const int panMul = params.panX2 ? 2 : 1;
    if (params.panX2) sfz << "pan_oncc10=200\n" << "pan_curvecc10=1\n";
    if (params.panAlternate)
        sfz << "pan_oncc137=" << (params.panRandom * panMul) << "\n"
            << "pan_curvecc137=1\n";
    else
        sfz << "pan_oncc136=" << (params.panRandom * panMul) << "\n"
            << "pan_curvecc136=1\n";
    // cc117 is reused as the volume_oncc117= modulation source (see
    // <control>'s set_cc117=127 and <global>'s volume_oncc117= above) - no
    // pan_curvecc117= is declared for it any more, so pan_oncc117 (Opcode
    // FX's Unison stereo width, further down) falls back to sfizz's default
    // curve.
    sfz << std::fixed << std::setprecision(5)
        << "lfo01_delay=" << params.panLfoDelay << "\n"
        << "lfo01_fade=" << params.panLfoFade << "\n"
        << "lfo01_pan=" << (params.panLfoPan * panMul) << "\n"
        << "lfo01_freq=" << params.panLfoFreq << "\n"
        << "lfo01_wave=" << params.panLfoWave << "\n";

    // Amp EG (fixed 6-breakpoint template) + amp LFO. At most 5 decimal
    // digits on every float opcode (user-specified, keeps the generated
    // text readable).
    sfz << "amp_keycenter=" << params.ampKeycenter << "\n"
        << "amp_keytrack=" << params.ampKeytrack << "\n"
        << "amp_veltrack=" << params.ampVeltrack << "\n"
        << "volume_oncc135=" << params.ampRandom << "\n";
    sfz << std::fixed << std::setprecision(5)
        << "eg01_ampeg=100\n"
        << "eg01_sustain=4\n"
        << "eg01_level0=" << params.ampStartLevel << " eg01_time0=-1\n"
        << "eg01_level1=" << params.ampStartLevel << " eg01_time1=" << params.ampDelayTime << "\n"
        << "eg01_level2=1 eg01_time2=" << params.ampAttackTime
        << " eg01_shape2=" << params.ampAttackShape << "\n"
        << "eg01_level3=1 eg01_time3=" << params.ampHoldTime << "\n"
        << "eg01_level4=" << params.ampSustainLevel << " eg01_time4=" << params.ampDecayTime
        << " eg01_shape4=" << params.ampDecayShape << "\n"
        << "eg01_level5=0 eg01_time5=" << params.ampReleaseTime
        << " eg01_shape5=" << params.ampReleaseShape << "\n"
        << "lfo02_delay=" << params.ampLfoDelay << "\n"
        << "lfo02_fade=" << params.ampLfoFade << "\n"
        << "lfo02_volume=" << params.ampLfoVolume << "\n"
        << "lfo02_freq=" << params.ampLfoFreq << "\n"
        << "lfo02_wave=" << params.ampLfoWave << "\n";

    // Filter: top-level opcodes, then the optional eg02_* envelope (only
    // when filterEgEnabled - unlike the always-on Amp eg01_*), then the
    // always-on filter LFO (lfo03_*). fil_veltrack's opcode NAME (not just
    // its value) depends on whether the filter EG is active - when it is,
    // velocity instead scales the envelope's cutoff depth.
    sfz << "filtype=" << params.filterType << "\n"
        << "cutoff=" << params.filterCutoff << "\n"
        << "resonance=" << params.filterResonance << "\n"
        << "cutoff_oncc135=" << params.filterRandomCutoff << "\n"
        << "fil_keycenter=" << params.filKeycenter << "\n"
        << "fil_keytrack=" << params.filKeytrack << "\n"
        << (params.filterEgEnabled ? "eg02_cutoff_oncc131=" : "cutoff_oncc131=")
        << params.filVeltrack << "\n"
        << "resonance_oncc131=" << params.resoVeltrack << "\n";

    if (params.filterEgEnabled) {
        sfz << "eg02_cutoff=" << params.filDepth << "\n"
            << "eg02_sustain=4\n"
            << "eg02_level0=" << params.filEgStartLevel << " eg02_time0=-1\n"
            << "eg02_level1=" << params.filEgStartLevel
            << " eg02_time1=" << params.filEgDelayTime << "\n"
            << "eg02_level2=1 eg02_time2=" << params.filEgAttackTime
            << " eg02_shape2=" << params.filEgAttackShape << "\n"
            << "eg02_level3=1 eg02_time3=" << params.filEgHoldTime << "\n"
            << "eg02_level4=" << params.filEgSustainLevel
            << " eg02_time4=" << params.filEgDecayTime
            << " eg02_shape4=" << params.filEgDecayShape << "\n"
            << "eg02_level5=0 eg02_time5=" << params.filEgReleaseTime
            << " eg02_shape5=" << params.filEgReleaseShape << "\n";
    }

    sfz << "lfo03_delay=" << params.filterLfoDelay << "\n"
        << "lfo03_fade=" << params.filterLfoFade << "\n"
        << "lfo03_cutoff=" << params.filterLfoDepth << "\n"
        << "lfo03_freq=" << params.filterLfoFreq << "\n"
        << "lfo03_wave=" << params.filterLfoWave << "\n";

    // Pitch: top-level opcodes, then the optional eg03_* envelope (only
    // when pitchEgEnabled, same convention as the Filter EG), then the
    // always-on pitch LFO (lfo04_*).
    sfz << "pitch_keytrack=" << params.pitchKeytrack << "\n"
        << "pitch_veltrack=" << params.pitchVeltrack << "\n"
        << "pitch_random=" << params.pitchRandom << "\n";

    // Portamento (eg04_*, fixed 2-breakpoint glide template) - only
    // eg04_time1 (glideTime) is user-configurable, the rest are fixed
    // literals per the SFZ portamento-via-envelope convention.
    if (params.portamentoEnabled) {
        sfz << "eg04_sustain=1\n"
            << "eg04_level0=-1\n"
            << "eg04_time0=0\n"
            << "eg04_pitch_oncc140=100\n"
            << "eg04_time1=" << params.glideTime << "\n"
            << "eg04_level1=0\n";
    }

    if (params.pitchEgEnabled) {
        sfz << "eg03_pitch=" << params.pitchDepth << "\n"
            << "eg03_sustain=4\n"
            << "eg03_level0=" << params.pitchEgStartLevel << " eg03_time0=-1\n"
            << "eg03_level1=" << params.pitchEgStartLevel
            << " eg03_time1=" << params.pitchEgDelayTime << "\n"
            << "eg03_level2=1 eg03_time2=" << params.pitchEgAttackTime
            << " eg03_shape2=" << params.pitchEgAttackShape << "\n"
            << "eg03_level3=1 eg03_time3=" << params.pitchEgHoldTime << "\n"
            << "eg03_level4=" << params.pitchEgSustainLevel
            << " eg03_time4=" << params.pitchEgDecayTime
            << " eg03_shape4=" << params.pitchEgDecayShape << "\n"
            << "eg03_level5=0 eg03_time5=" << params.pitchEgReleaseTime
            << " eg03_shape5=" << params.pitchEgReleaseShape << "\n";
    }

    sfz << "lfo04_delay=" << params.pitchLfoDelay << "\n"
        << "lfo04_fade=" << params.pitchLfoFade << "\n"
        << "lfo04_pitch=" << params.pitchLfoPitch << "\n"
        << "lfo04_freq=" << params.pitchLfoFreq << "\n"
        << "lfo04_wave=" << params.pitchLfoWave << "\n";

    // Opcodes tab: raw user text, appended verbatim after every other
    // <global> opcode above.
    if (!params.customOpcodes.empty())
        sfz << params.customOpcodes << "\n";

    // FX tab ("Opcode FX"): normally the stack's regions as-is; when
    // fxEnabled the same stack is instead repeated across 2-3 <group>s
    // ("oscillators") per the selected mode, each adding its own delay/
    // detune/pan/chorus-LFO opcodes on top of everything already written
    // into <global> above (SFZ's usual global -> group -> region
    // inheritance still applies, so every group still gets the Amp/Filter/
    // Pitch/etc. config from above).
    auto emitRegion = [&]() {
        if (params.stack.empty()) {
            sfz << "<region>\n";
            return;
        }
        for (const auto& item : params.stack) {
            if (item.isSfz) {
                sfz << item.regionsText;
            } else {
                sfz << "<region>\n";
                if (!item.sampleRelativePath.empty())
                    sfz << "sample=" << item.sampleRelativePath << "\n";
            }
        }
    };

    if (!params.fxEnabled) {
        emitRegion();
        return sfz.str();
    }

    auto resolveCc = [](int ccMode, int defaultCc) {
        switch (ccMode) {
            case 1: return 135;
            case 2: return 136;
            case 3: return 137;
            default: return defaultCc;
        }
    };
    const int detuneCc = resolveCc(params.fxDetuneCcMode, 90);
    const int delayCc = resolveCc(params.fxDelayCcMode, 89);
    const int phaseCc = resolveCc(params.fxPhaseCcMode, 116);

    // lfoNN_* chorus modulation - same depth/speed/wave/phase on every "wet"
    // oscillator that uses it, only the LFO index and tune_oncc value can
    // differ between them (see emitChorusStereoPair below).
    auto emitChorusLfo = [&](int lfoIndex) {
        sfz << "lfo0" << lfoIndex << "_pitch=" << params.fxDepth << "\n"
            << "lfo0" << lfoIndex << "_freq=" << params.fxSpeed << "\n"
            << "lfo0" << lfoIndex << "_wave=" << params.fxWave << "\n"
            << "lfo0" << lfoIndex << "_phase_oncc" << phaseCc << "=" << params.fxPhase << "\n";
    };

    // Oscillators 1+2 of Chorus Stereo (Wet) - also the first two groups of
    // Chorus Stereo (Wet+Dry), which just adds a third, unmodulated group.
    // Oscillator 1 always drives lfo05; oscillator 2 shares that same lfo05
    // unless fxIndependentLfo asks for the two oscillators to run fully
    // independent LFOs, in which case it gets lfo06 instead.
    //
    // applyFil2: writes fil2type=/cutoff2=/volume_oncc117= into both
    // oscillators - only passed true from Chorus Stereo (Wet+Dry) (per user
    // spec, the second filter/makeup gain contrasts the wet chorus pair
    // against the plain dry group added alongside it); Chorus Stereo (Wet)
    // has no dry counterpart to contrast against, so it never gets them.
    // volume_oncc117= is sample_volume+fxVolume (see shared.hpp's fxVolume)
    // - both are dB values that stack (e.g. sample volume=-6, fxVolume=-3
    // writes -9). The hard pan=100/-100 below is doubled by panMul (see the
    // Pan section above) same as every other pan opcode this file writes.
    auto emitChorusStereoPair = [&](bool applyFil2) {
        sfz << "<group>\n"
            << "delay_oncc" << delayCc << "=" << params.fxDelay << "\n"
            << "tune_oncc" << detuneCc << "=" << params.fxDetune << "\n";
        emitChorusLfo(5);
        if (applyFil2)
            sfz << "fil2type=" << params.fil2Type << "\n"
                << "cutoff2=" << params.cutoff2 << "\n"
                << "volume_oncc117=" << (params.volume + params.fxVolume) << "\n";
        sfz << "pan=" << (100 * panMul) << "\n";
        emitRegion();

        sfz << "<group>\n"
            << "tune_oncc" << detuneCc << "=" << -params.fxDetune << "\n";
        emitChorusLfo(params.fxIndependentLfo ? 6 : 5);
        if (applyFil2)
            sfz << "fil2type=" << params.fil2Type << "\n"
                << "cutoff2=" << params.cutoff2 << "\n"
                << "volume_oncc117=" << (params.volume + params.fxVolume) << "\n";
        sfz << "pan=" << (-100 * panMul) << "\n";
        emitRegion();
    };

    switch (params.fxMode) {
        case 0: // Unison
            sfz << "<group>\n"
                << "delay_oncc" << delayCc << "=" << params.fxDelay << "\n"
                << "tune_oncc" << detuneCc << "=" << params.fxDetune << "\n"
                << "pan_oncc117=" << (params.fxStereoWidth * panMul) << "\n";
            emitRegion();

            sfz << "<group>\n"
                << "tune_oncc" << detuneCc << "=" << -params.fxDetune << "\n"
                << "pan_oncc117=" << (-params.fxStereoWidth * panMul) << "\n";
            emitRegion();
            break;

        case 1: // Chorus Mono
            sfz << "<group>\n"
                << "delay_oncc" << delayCc << "=" << params.fxDelay << "\n"
                << "tune_oncc" << detuneCc << "=" << params.fxDetune << "\n";
            emitChorusLfo(5);
            sfz << "fil2type=" << params.fil2Type << "\n"
                << "cutoff2=" << params.cutoff2 << "\n"
                << "volume_oncc117=" << (params.volume + params.fxVolume) << "\n";
            emitRegion();

            sfz << "<group>\n";
            emitRegion();
            break;

        case 2: // Chorus Stereo (Wet)
            emitChorusStereoPair(false);
            break;

        case 3: // Chorus Stereo (Wet+Dry)
            emitChorusStereoPair(true);
            sfz << "<group>\n";
            emitRegion();
            break;

        default:
            emitRegion();
            break;
    }

    return sfz.str();
}
