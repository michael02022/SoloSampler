#include "PresetFile.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace {

constexpr uint32_t kPresetMagic = 0x46505353;   // "SSPF" (SoloSampler Preset File) LE bytes
constexpr uint32_t kPresetFileVersion = 3; // v3: samplePath/multisample* -> a variable-length stack
// Independent of plugin.cpp's kStateMagic/kStateVersion (the CLAP host
// state format) - these two formats must never be conflated, even though
// they happen to carry mostly the same fields.

template <typename T>
bool writeVal(std::ofstream& s, const T& v) {
    s.write(reinterpret_cast<const char*>(&v), sizeof(T));
    return static_cast<bool>(s);
}
template <typename T>
bool readVal(std::ifstream& s, T& v) {
    s.read(reinterpret_cast<char*>(&v), sizeof(T));
    return static_cast<bool>(s);
}

bool writeStr(std::ofstream& s, const std::string& str) {
    if (!writeVal(s, static_cast<uint32_t>(str.size()))) return false;
    if (str.empty()) return true;
    s.write(str.data(), static_cast<std::streamsize>(str.size()));
    return static_cast<bool>(s);
}
bool readStr(std::ifstream& s, std::string& str) {
    uint32_t len = 0;
    if (!readVal(s, len)) return false;
    str.resize(len);
    if (len == 0) return true;
    s.read(str.data(), static_cast<std::streamsize>(len));
    return static_cast<bool>(s);
}

bool writeBool(std::ofstream& s, bool b) { return writeVal(s, static_cast<uint8_t>(b ? 1 : 0)); }
bool readBool(std::ifstream& s, bool& b) {
    uint8_t v = 0;
    if (!readVal(s, v)) return false;
    b = v != 0;
    return true;
}

constexpr uint32_t kExternalBlobMagic = 0x58455353; // "SSEX" LE bytes

// External tab settings: magic + byte size + the raw ExternalSettings
// struct, appended after everything else. Reading copies at most
// sizeof(ExternalSettings) bytes over a default-constructed struct, so a
// shorter blob from an older build keeps its newer fields at their
// defaults - and a file with no blob at all (written before the External
// tab existed) still loads.
bool writeExternalBlob(std::ofstream& s, const ExternalSettings& e) {
    if (!writeVal(s, kExternalBlobMagic) ||
        !writeVal(s, static_cast<uint32_t>(sizeof(ExternalSettings))))
        return false;
    s.write(reinterpret_cast<const char*>(&e), sizeof(ExternalSettings));
    return static_cast<bool>(s);
}
void readExternalBlob(std::ifstream& s, ExternalSettings& e) {
    uint32_t magic = 0, size = 0;
    if (!readVal(s, magic) || magic != kExternalBlobMagic || !readVal(s, size)) return;
    std::string bytes(size, '\0');
    s.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!s) return;
    ExternalSettings loaded;
    std::memcpy(&loaded, bytes.data(), std::min<size_t>(size, sizeof(ExternalSettings)));
    e = loaded;
}

} // namespace

bool writePresetFile(const std::string& path, int kind, const PresetFields& f) {
    std::ofstream s(path, std::ios::binary | std::ios::trunc);
    if (!s) return false;

    bool ok = writeVal(s, kPresetMagic) && writeVal(s, kPresetFileVersion) &&
             writeVal(s, static_cast<uint32_t>(kind)) &&
             writeVal(s, f.rootNote) && writeVal(s, f.volume) &&
             writeVal(s, f.bendUpCents) && writeVal(s, f.bendDownCents) &&
             writeVal(s, f.quality) && writeVal(s, f.loopModeIndex) &&
             writeVal(s, f.polyphony) && writeVal(s, f.notePolyphony) &&
             writeVal(s, f.tuneCents) && writeBool(s, f.offsetEnabled) &&
             writeVal(s, f.offsetValue) && writeBool(s, f.randomOffsetEnabled) &&
             writeVal(s, f.panRandom) && writeBool(s, f.panAlternate) &&
             writeVal(s, f.panLfoDelay) && writeVal(s, f.panLfoFade) &&
             writeVal(s, f.panLfoPan) && writeVal(s, f.panLfoFreq) &&
             writeVal(s, f.panLfoWave) && writeVal(s, f.ampKeycenter) &&
             writeVal(s, f.ampKeytrack) && writeVal(s, f.ampVeltrack) &&
             writeVal(s, f.ampRandom) && writeVal(s, f.ampStartLevel) &&
             writeVal(s, f.ampDelayTime) && writeVal(s, f.ampAttackTime) &&
             writeVal(s, f.ampAttackShape) && writeVal(s, f.ampHoldTime) &&
             writeVal(s, f.ampDecayTime) && writeVal(s, f.ampDecayShape) &&
             writeVal(s, f.ampSustainLevel) && writeVal(s, f.ampReleaseTime) &&
             writeVal(s, f.ampReleaseShape) && writeVal(s, f.ampLfoDelay) &&
             writeVal(s, f.ampLfoFade) && writeVal(s, f.ampLfoVolume) &&
             writeVal(s, f.ampLfoFreq) && writeVal(s, f.ampLfoWave) &&
             writeVal(s, f.filterTypeIndex) && writeVal(s, f.filterCutoff) &&
             writeVal(s, f.filterResonance) && writeVal(s, f.filterRandomCutoff) &&
             writeVal(s, f.filKeycenter) && writeVal(s, f.filKeytrack) &&
             writeVal(s, f.filVeltrack) && writeVal(s, f.resoVeltrack) &&
             writeBool(s, f.filterEgEnabled) && writeVal(s, f.filDepth) &&
             writeVal(s, f.filEgStartLevel) && writeVal(s, f.filEgDelayTime) &&
             writeVal(s, f.filEgAttackTime) && writeVal(s, f.filEgAttackShape) &&
             writeVal(s, f.filEgHoldTime) && writeVal(s, f.filEgDecayTime) &&
             writeVal(s, f.filEgDecayShape) && writeVal(s, f.filEgSustainLevel) &&
             writeVal(s, f.filEgReleaseTime) && writeVal(s, f.filEgReleaseShape) &&
             writeVal(s, f.filterLfoDelay) && writeVal(s, f.filterLfoFade) &&
             writeVal(s, f.filterLfoDepth) && writeVal(s, f.filterLfoFreq) &&
             writeVal(s, f.filterLfoWave) && writeVal(s, f.pitchKeytrack) &&
             writeVal(s, f.pitchVeltrack) && writeVal(s, f.pitchRandom) &&
             writeBool(s, f.portamentoEnabled) && writeVal(s, f.glideTime) &&
             writeBool(s, f.pitchEgEnabled) && writeVal(s, f.pitchDepth) &&
             writeVal(s, f.pitchEgStartLevel) && writeVal(s, f.pitchEgDelayTime) &&
             writeVal(s, f.pitchEgAttackTime) && writeVal(s, f.pitchEgAttackShape) &&
             writeVal(s, f.pitchEgHoldTime) && writeVal(s, f.pitchEgDecayTime) &&
             writeVal(s, f.pitchEgDecayShape) && writeVal(s, f.pitchEgSustainLevel) &&
             writeVal(s, f.pitchEgReleaseTime) && writeVal(s, f.pitchEgReleaseShape) &&
             writeVal(s, f.pitchLfoDelay) && writeVal(s, f.pitchLfoFade) &&
             writeVal(s, f.pitchLfoPitch) && writeVal(s, f.pitchLfoFreq) &&
             writeVal(s, f.pitchLfoWave) && writeVal(s, f.ccVolume) &&
             writeVal(s, f.ccPan) && writeBool(s, f.fxEnabled) &&
             writeVal(s, f.fxMode) && writeVal(s, f.fxDetune) &&
             writeVal(s, f.fxDetuneCcMode) && writeVal(s, f.fxDelay) &&
             writeVal(s, f.fxDelayCcMode) && writeVal(s, f.fxStereoWidth) &&
             writeVal(s, f.fxDepth) && writeVal(s, f.fxSpeed) &&
             writeVal(s, f.fxWave) && writeVal(s, f.fxPhase) &&
             writeVal(s, f.fxPhaseCcMode) && writeBool(s, f.fxIndependentLfo) &&
             writeVal(s, f.fil2TypeIndex) && writeVal(s, f.cutoff2) &&
             writeVal(s, f.fxVolume) && writeBool(s, f.reverbEnabled) &&
             writeVal(s, f.reverbTypeIndex) && writeVal(s, f.reverbInput) &&
             writeVal(s, f.reverbPredelay) && writeVal(s, f.reverbSize) &&
             writeVal(s, f.reverbTone) && writeVal(s, f.reverbDamp) &&
             writeVal(s, f.reverbDry) && writeVal(s, f.reverbWet) &&
             writeStr(s, f.customOpcodesText) && writeBool(s, f.mpeEnabled) &&
             writeVal(s, f.character) && writeVal(s, f.multisampleRootNote) &&
             writeBool(s, f.panX2);

    if (ok && kind == 0) {
        ok = writeVal(s, static_cast<uint32_t>(f.stack.size()));
        for (const auto& item : f.stack) {
            if (!ok) break;
            ok = writeStr(s, item.path) && writeBool(s, item.isSfz) &&
                writeStr(s, item.regionsText);
        }
    }
    ok = ok && writeExternalBlob(s, f.external);

    s.flush();
    return ok && static_cast<bool>(s);
}

PresetFileResult readPresetFile(const std::string& path) {
    PresetFileResult result;
    std::ifstream s(path, std::ios::binary);
    if (!s) {
        result.error = "Could not open file: " + path;
        return result;
    }

    uint32_t magic = 0, version = 0, kind = 0;
    if (!readVal(s, magic) || magic != kPresetMagic) {
        result.error = "Not a SoloSampler preset/profile file: " + path;
        return result;
    }
    if (!readVal(s, version) || version != kPresetFileVersion) {
        result.error = "Unsupported preset/profile file version: " + path;
        return result;
    }
    if (!readVal(s, kind)) {
        result.error = "Corrupt preset/profile file: " + path;
        return result;
    }

    PresetFields& f = result.fields;
    bool ok = readVal(s, f.rootNote) && readVal(s, f.volume) &&
             readVal(s, f.bendUpCents) && readVal(s, f.bendDownCents) &&
             readVal(s, f.quality) && readVal(s, f.loopModeIndex) &&
             readVal(s, f.polyphony) && readVal(s, f.notePolyphony) &&
             readVal(s, f.tuneCents) && readBool(s, f.offsetEnabled) &&
             readVal(s, f.offsetValue) && readBool(s, f.randomOffsetEnabled) &&
             readVal(s, f.panRandom) && readBool(s, f.panAlternate) &&
             readVal(s, f.panLfoDelay) && readVal(s, f.panLfoFade) &&
             readVal(s, f.panLfoPan) && readVal(s, f.panLfoFreq) &&
             readVal(s, f.panLfoWave) && readVal(s, f.ampKeycenter) &&
             readVal(s, f.ampKeytrack) && readVal(s, f.ampVeltrack) &&
             readVal(s, f.ampRandom) && readVal(s, f.ampStartLevel) &&
             readVal(s, f.ampDelayTime) && readVal(s, f.ampAttackTime) &&
             readVal(s, f.ampAttackShape) && readVal(s, f.ampHoldTime) &&
             readVal(s, f.ampDecayTime) && readVal(s, f.ampDecayShape) &&
             readVal(s, f.ampSustainLevel) && readVal(s, f.ampReleaseTime) &&
             readVal(s, f.ampReleaseShape) && readVal(s, f.ampLfoDelay) &&
             readVal(s, f.ampLfoFade) && readVal(s, f.ampLfoVolume) &&
             readVal(s, f.ampLfoFreq) && readVal(s, f.ampLfoWave) &&
             readVal(s, f.filterTypeIndex) && readVal(s, f.filterCutoff) &&
             readVal(s, f.filterResonance) && readVal(s, f.filterRandomCutoff) &&
             readVal(s, f.filKeycenter) && readVal(s, f.filKeytrack) &&
             readVal(s, f.filVeltrack) && readVal(s, f.resoVeltrack) &&
             readBool(s, f.filterEgEnabled) && readVal(s, f.filDepth) &&
             readVal(s, f.filEgStartLevel) && readVal(s, f.filEgDelayTime) &&
             readVal(s, f.filEgAttackTime) && readVal(s, f.filEgAttackShape) &&
             readVal(s, f.filEgHoldTime) && readVal(s, f.filEgDecayTime) &&
             readVal(s, f.filEgDecayShape) && readVal(s, f.filEgSustainLevel) &&
             readVal(s, f.filEgReleaseTime) && readVal(s, f.filEgReleaseShape) &&
             readVal(s, f.filterLfoDelay) && readVal(s, f.filterLfoFade) &&
             readVal(s, f.filterLfoDepth) && readVal(s, f.filterLfoFreq) &&
             readVal(s, f.filterLfoWave) && readVal(s, f.pitchKeytrack) &&
             readVal(s, f.pitchVeltrack) && readVal(s, f.pitchRandom) &&
             readBool(s, f.portamentoEnabled) && readVal(s, f.glideTime) &&
             readBool(s, f.pitchEgEnabled) && readVal(s, f.pitchDepth) &&
             readVal(s, f.pitchEgStartLevel) && readVal(s, f.pitchEgDelayTime) &&
             readVal(s, f.pitchEgAttackTime) && readVal(s, f.pitchEgAttackShape) &&
             readVal(s, f.pitchEgHoldTime) && readVal(s, f.pitchEgDecayTime) &&
             readVal(s, f.pitchEgDecayShape) && readVal(s, f.pitchEgSustainLevel) &&
             readVal(s, f.pitchEgReleaseTime) && readVal(s, f.pitchEgReleaseShape) &&
             readVal(s, f.pitchLfoDelay) && readVal(s, f.pitchLfoFade) &&
             readVal(s, f.pitchLfoPitch) && readVal(s, f.pitchLfoFreq) &&
             readVal(s, f.pitchLfoWave) && readVal(s, f.ccVolume) &&
             readVal(s, f.ccPan) && readBool(s, f.fxEnabled) &&
             readVal(s, f.fxMode) && readVal(s, f.fxDetune) &&
             readVal(s, f.fxDetuneCcMode) && readVal(s, f.fxDelay) &&
             readVal(s, f.fxDelayCcMode) && readVal(s, f.fxStereoWidth) &&
             readVal(s, f.fxDepth) && readVal(s, f.fxSpeed) &&
             readVal(s, f.fxWave) && readVal(s, f.fxPhase) &&
             readVal(s, f.fxPhaseCcMode) && readBool(s, f.fxIndependentLfo) &&
             readVal(s, f.fil2TypeIndex) && readVal(s, f.cutoff2) &&
             readVal(s, f.fxVolume) && readBool(s, f.reverbEnabled) &&
             readVal(s, f.reverbTypeIndex) && readVal(s, f.reverbInput) &&
             readVal(s, f.reverbPredelay) && readVal(s, f.reverbSize) &&
             readVal(s, f.reverbTone) && readVal(s, f.reverbDamp) &&
             readVal(s, f.reverbDry) && readVal(s, f.reverbWet) &&
             readStr(s, f.customOpcodesText) && readBool(s, f.mpeEnabled) &&
             readVal(s, f.character) && readVal(s, f.multisampleRootNote) &&
             readBool(s, f.panX2);

    if (ok && kind == 0) {
        uint32_t stackCount = 0;
        ok = readVal(s, stackCount);
        for (uint32_t i = 0; ok && i < stackCount; ++i) {
            PresetStackItem item;
            ok = readStr(s, item.path) && readBool(s, item.isSfz) &&
                readStr(s, item.regionsText);
            if (ok) f.stack.push_back(std::move(item));
        }
    }
    if (ok) readExternalBlob(s, f.external); // optional, see readExternalBlob

    if (!ok) {
        result.error = "Corrupt or truncated preset/profile file: " + path;
        result.fields = PresetFields{};
        return result;
    }

    result.ok = true;
    result.kind = static_cast<int>(kind);
    return result;
}
