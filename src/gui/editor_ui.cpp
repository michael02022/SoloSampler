#include "editor_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // ImGuiInputTextState - see drawOpcodesTab's context menu
#include "state/SampleInfo.h"

namespace {

// Extensions accepted by the sample/SFZ file dialog - a plain sample or an
// .sfz can both be picked from the same dialog (plugin.cpp's loadStack
// dispatches on extension).
std::vector<std::string> loadDialogExtensions() {
    std::vector<std::string> exts(kSupportedAudioExtensions.begin(),
                                  kSupportedAudioExtensions.end());
    exts.push_back(".sfz");
    return exts;
}

// True once the Sample tab's stack contains at least one .sfz mapping -
// gates Character/the piano's right-click target, same rationale the old
// multisampleEnabled checkbox had, just derived from the stack's actual
// contents instead of set manually. GUI thread only.
bool hasSfzInStack(SharedParams& params) {
    std::lock_guard<std::mutex> lock(params.guiState.mutex);
    for (const auto& item : params.guiState.stack)
        if (item.isSfz) return true;
    return false;
}

std::string noteName(int n) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B"};
    int octave = n / 12 - 1;
    return std::string(kNames[n % 12]) + std::to_string(octave);
}

// Extra interactions layered on every slider in this UI: middle-click opens
// a popup to type an exact value, right-click resets to defaultValue.
// Must be called immediately after the Slider*/VSlider* widget so
// IsItemClicked/IsItemHovered still refer to it. popupId must be unique per
// slider (call sites use the widget's own "##id" string + a suffix).
bool sliderExtrasFloat(const char* popupId, float* value, float defaultValue, float lo, float hi) {
    bool changed = false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
        ImGui::OpenPopup(popupId);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *value = defaultValue;
        changed = true;
    }
    if (ImGui::BeginPopup(popupId)) {
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("##edit", value, 0.0f, 0.0f, "%.5f",
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
            *value = std::clamp(*value, lo, hi);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}

bool sliderExtrasInt(const char* popupId, int* value, int defaultValue, int lo, int hi) {
    bool changed = false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
        ImGui::OpenPopup(popupId);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *value = defaultValue;
        changed = true;
    }
    if (ImGui::BeginPopup(popupId)) {
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputInt("##edit", value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
            *value = std::clamp(*value, lo, hi);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}

// ImGui::InputTextMultiline only takes a fixed char buffer - imgui_stdlib.h
// (the official std::string adapter) isn't vendored in this project, so
// this reimplements its resize-callback trick directly: ImGui is given
// str's own buffer (capacity()+1 bytes) and, whenever the user types past
// that capacity, ImGuiInputTextFlags_CallbackResize fires and the callback
// grows str before ImGui writes further.
bool inputTextMultilineStdString(const char* label, std::string* str, const ImVec2& size) {
    auto callback = [](ImGuiInputTextCallbackData* data) -> int {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(data->UserData);
            s->resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = s->data();
        }
        return 0;
    };
    return ImGui::InputTextMultiline(label, str->data(), str->capacity() + 1, size,
                                     ImGuiInputTextFlags_CallbackResize, callback, str);
}

// A vertical fader with a short label above it. Returns true (with *value
// updated) on any frame the user changed it - caller stores it back into
// the atomic and fires onParamChanged, same immediate-apply convention as
// every other control in this UI (e.g. the tune/offset sliders). Also gets
// the middle-click-to-type/right-click-to-reset extras every slider in
// this UI has (see sliderExtrasFloat above).
bool vFader(const char* label, const char* imguiId, float* value, float lo, float hi,
           float defaultValue) {
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 52.f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    bool changed = ImGui::VSliderFloat(imguiId, ImVec2(44.f, 140.f), value, lo, hi, "%.5f");
    std::string popupId = std::string(imguiId) + "popup";
    changed |= sliderExtrasFloat(popupId.c_str(), value, defaultValue, lo, hi);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, 10.f);
    return changed;
}

// Binds a vFader directly to an atomic<float> (load/vFader/store/notify) so
// drawPanTab/drawAmpTab/drawFilterTab/drawPitchTab's EG/LFO rows can stay
// one line per fader instead of repeating that boilerplate at every call
// site.
void atomicVFader(std::atomic<float>& param, const char* label, const char* imguiId, float lo,
             float hi, float defaultValue, const std::function<void()>& onParamChanged) {
    float v = param.load();
    if (vFader(label, imguiId, &v, lo, hi, defaultValue)) {
        param = v;
        if (onParamChanged) onParamChanged();
    }
}

// Horizontal counterpart to atomicVFader, used for the EG shape ("curve")
// sliders - pulled out of the vertical EG fader row (see drawAmpTab/
// drawFilterTab/drawPitchTab) into their own horizontal group underneath so
// they read as distinct "fine-tuning" controls rather than blending in with
// the main Level/Time faders.
void atomicHSliderFloat(std::atomic<float>& param, const char* label, const char* popupId,
                        float lo, float hi, float defaultValue,
                        const std::function<void()>& onParamChanged) {
    ImGui::SetNextItemWidth(200);
    float v = param.load();
    bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.5f");
    changed |= sliderExtrasFloat(popupId, &v, defaultValue, lo, hi);
    if (changed) {
        param = v;
        if (onParamChanged) onParamChanged();
    }
}

// Same idea as vFader/atomicVFader combined, for the int-ranged vertical
// faders (LFO Pan, FIL_DEPTH, the filter/pitch LFOs' depth slots) - their
// ranges are too large/coarse to be float-with-5-decimals like the rest of
// an EG row.
void intVFader(std::atomic<int>& param, const char* label, const char* imguiId, int lo, int hi,
              int defaultValue, const std::function<void()>& onParamChanged) {
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 52.f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    int v = param.load();
    bool changed = ImGui::VSliderInt(imguiId, ImVec2(44.f, 140.f), &v, lo, hi);
    std::string popupId = std::string(imguiId) + "popup";
    changed |= sliderExtrasInt(popupId.c_str(), &v, defaultValue, lo, hi);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, 10.f);
    if (changed) {
        param = v;
        if (onParamChanged) onParamChanged();
    }
}

// lfoNN_wave vertical fader: shows the wave name as a tooltip + label below
// (raw 0-7 alone isn't meaningful). Shared by every tab's LFO row - safe to
// reuse the same literal widget ids across all of them since each lives in
// its own BeginChild scope (e.g. Pan's "##panlfo" vs. Amp's "##amplfo"),
// which already disambiguates the hashed ImGui ID.
void lfoWaveFader(std::atomic<int>& param, const std::function<void()>& onParamChanged,
                  int defaultWave = 1) {
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 52.f);
    ImGui::TextUnformatted("LFO Wave");
    ImGui::PopTextWrapPos();
    int wave = param.load();
    bool changed = ImGui::VSliderInt("##lfowave", ImVec2(44.f, 140.f), &wave, 0, 7);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kLfoWaveNames[wave]);
    changed |= sliderExtrasInt("##lfowavepopup", &wave, defaultWave, 0, 7);
    if (changed) {
        param = wave;
        if (onParamChanged) onParamChanged();
    }
    ImGui::TextUnformatted(kLfoWaveNames[wave]);
    ImGui::EndGroup();
}

// ------------------------------------------------------------- piano/tune

void drawPiano(SharedParams& params, EditorUIState& ui, float width,
               const std::function<void()>& onParamChanged) {
    const float pianoH = 88.f;
    const float whiteW = 13.f;
    static const bool kBlackMap[12] = {false, true, false, true, false, false,
                                       true, false, true, false, true, false};
    const float contentW = 76 * whiteW; // 128 MIDI notes: 75 white + margin

    ImGui::BeginChild("##piano",
                      ImVec2(width, pianoH + ImGui::GetStyle().ScrollbarSize + 4),
                      false, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pianokeys", ImVec2(contentW, pianoH),
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    // Once the stack has an .sfz in it, pitch_keycenter= is normally driven
    // per-region by the imported mapping (region opcodes win over <global>),
    // so the right-click gesture below targets multisampleRootNote (a
    // relative transpose= nudge) instead of the plain rootNote it otherwise
    // sets - see SharedParams::multisampleRootNote for the full rationale.
    const bool multisampleMode = hasSfzInStack(params);
    const int root =
        multisampleMode ? params.multisampleRootNote.load() : params.rootNote.load();

    struct Key { float x0, x1; bool black; };
    static Key keys[128];
    static bool geomDone = false;
    if (!geomDone) {
        geomDone = true;
        int whiteIdx = 0;
        for (int n = 0; n < 128; ++n) {
            if (!kBlackMap[n % 12]) {
                keys[n] = {whiteIdx * whiteW, (whiteIdx + 1) * whiteW, false};
                ++whiteIdx;
            } else {
                float cx = whiteIdx * whiteW;
                keys[n] = {cx - whiteW * 0.32f, cx + whiteW * 0.32f, true};
            }
        }
    }

    // hit-test: black keys first (they're drawn on top)
    int hitNote = -1;
    float hitRelY = 0.f;
    if (hovered) {
        float mx = mouse.x - p0.x, my = mouse.y - p0.y;
        for (int n = 0; n < 128 && hitNote < 0; ++n)
            if (keys[n].black && my < pianoH * 0.62f && mx >= keys[n].x0 &&
                mx < keys[n].x1) {
                hitNote = n;
                hitRelY = my / (pianoH * 0.62f);
            }
        for (int n = 0; n < 128 && hitNote < 0; ++n)
            if (!keys[n].black && mx >= keys[n].x0 && mx < keys[n].x1) {
                hitNote = n;
                hitRelY = my / pianoH;
            }
    }

    // press: velocity from vertical click position - bottom of the key =
    // loud, top = soft (user-specified convention for this project).
    if (hitNote >= 0 && ui.heldPianoKey < 0 && ImGui::IsMouseClicked(0)) {
        int velocity = 1 + static_cast<int>(
                               std::lround(std::clamp(hitRelY, 0.f, 1.f) * 126.0f));
        ui.heldPianoKey = hitNote;
        params.previewNoteOnNumber = hitNote;
        params.previewNoteOnVelocity = velocity;
        params.pendingPreviewNoteOn = true;
    }
    if (hitNote >= 0 && ImGui::IsMouseClicked(1)) {
        if (multisampleMode)
            params.multisampleRootNote = hitNote;
        else
            params.rootNote = hitNote;
        if (onParamChanged) onParamChanged();
    }

    // release: checked against global mouse state (not item hover), so
    // dragging off the keyboard while still holding the button still
    // releases the note instead of leaving it stuck on.
    if (ui.heldPianoKey >= 0 && !ImGui::IsMouseDown(0)) {
        params.previewNoteOffNumber = ui.heldPianoKey;
        params.pendingPreviewNoteOff = true;
        ui.heldPianoKey = -1;
    }

    // draw: white keys, then black
    for (int pass = 0; pass < 2; ++pass) {
        for (int n = 0; n < 128; ++n) {
            const Key& k = keys[n];
            if (k.black != (pass == 1)) continue;
            float x0 = p0.x + k.x0, x1 = p0.x + k.x1;
            float y0 = p0.y, y1 = p0.y + (k.black ? pianoH * 0.62f : pianoH);
            bool active = params.noteActive[static_cast<size_t>(n)].load();
            ImU32 col;
            if (n == root)
                col = IM_COL32(240, 150, 40, 255); // root note, marked
            else if (active)
                col = IM_COL32(120, 190, 255, 255);
            else if (n == hitNote)
                col = k.black ? IM_COL32(90, 90, 100, 255)
                              : IM_COL32(230, 230, 235, 255);
            else
                col = k.black ? IM_COL32(25, 25, 30, 255)
                              : IM_COL32(245, 245, 248, 255);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
            if (!k.black)
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(90, 90, 95, 255));
            if (active) {
                float v = params.noteVelocity01[static_cast<size_t>(n)].load();
                float barTop = y0 + (y1 - y0) * (1.0f - v);
                dl->AddRectFilled(ImVec2(x0, barTop), ImVec2(x1, y1),
                                  IM_COL32(220, 60, 60, 140));
            }
            if (n % 12 == 0 && !k.black)
                dl->AddText(ImVec2(x0 + 1, p0.y + pianoH - 16),
                            IM_COL32(90, 90, 95, 255), noteName(n).c_str());
        }
    }

    if (hitNote >= 0) {
        ImGui::SetTooltip("%s (%d)%s", noteName(hitNote).c_str(), hitNote,
                          hitNote == root ? " - root note" : "");
    }

    if (!ui.pianoScrolledOnce) {
        ui.pianoScrolledOnce = true;
        ImGui::SetScrollX(std::max(0.f, keys[root].x0 - 300.f));
    }

    ImGui::EndChild();
}

void drawTuneAndPiano(SharedParams& params, EditorUIState& ui, float width,
                      const std::function<void()>& onParamChanged) {
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(width);
    float cents = params.tuneCents.load();
    int centsInt = static_cast<int>(std::lround(cents));
    bool tuneChanged = ImGui::SliderInt("##tune", &centsInt, -100, 100, "Tune: %d cents",
                                        ImGuiSliderFlags_AlwaysClamp);
    tuneChanged |= sliderExtrasInt("##tunepopup", &centsInt, 0, -100, 100);
    if (tuneChanged) {
        params.tuneCents = static_cast<float>(centsInt);
        if (onParamChanged) onParamChanged();
    }
    drawPiano(params, ui, width, onParamChanged);
    // Live voice count (SfizzEngine::activeVoiceCount, mirrored every block
    // in plugProcess - see shared.hpp's activeVoiceCount) against the
    // configured Polyphony ceiling, for reference.
    ImGui::Text("Voices: %d / %d", params.activeVoiceCount.load(), params.polyphony.load());
    ImGui::EndGroup();
}

// -------------------------------------------------------------- sample tab

void drawSampleTab(SharedParams& params, EditorUIState& ui,
                   const std::function<void()>& onParamChanged,
                   const std::function<void(size_t)>& onRemoveStackItem,
                   const std::function<void()>& onClearStack) {
    static constexpr int kBendChoices[4] = {200, 1200, 2400, 4800};
    int bendUp = params.bendUpCents.load();
    int bendIdx = 2; // default 2400
    for (int i = 0; i < 4; ++i)
        if (kBendChoices[i] == bendUp) { bendIdx = i; break; }

    char bendLabel[8];
    std::snprintf(bendLabel, sizeof(bendLabel), "%d", kBendChoices[bendIdx]);
    if (ImGui::BeginCombo("Bend Range", bendLabel)) {
        for (int i = 0; i < 4; ++i) {
            bool sel = (i == bendIdx);
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%d", kBendChoices[i]);
            if (ImGui::Selectable(lbl, sel)) {
                params.bendUpCents = kBendChoices[i];
                params.bendDownCents = -kBendChoices[i];
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Engine-level toggle (sfizz_set_mpe_enabled, applied on the audio
    // thread - see SfizzEngine::renderBlock) - not itself an SFZ opcode, so
    // no onParamChanged/SFZ regeneration needed for the flag alone. Checking
    // it DOES also force Bend Range above to 4800 cents (MPE 1.0's 48-
    // semitone per-note pitch bend convention - see SfzDocument.cpp's
    // bendup=/benddown=), which is an opcode change and does need
    // onParamChanged; unchecking leaves Bend Range as the user left it.
    bool mpeEnabled = params.mpeEnabled.load();
    if (ImGui::Checkbox("Enable MPE", &mpeEnabled)) {
        params.mpeEnabled = mpeEnabled;
        if (mpeEnabled) {
            params.bendUpCents = 4800;
            params.bendDownCents = -4800;
            if (onParamChanged) onParamChanged();
        }
    }

    int quality = params.quality.load();
    char qLabel[8];
    std::snprintf(qLabel, sizeof(qLabel), "%d", quality);
    if (ImGui::BeginCombo("Quality", qLabel)) {
        for (int i = 0; i <= 10; ++i) {
            bool sel = (i == quality);
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%d", i);
            if (ImGui::Selectable(lbl, sel)) {
                params.quality = i;
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    int loopIdx = params.loopModeIndex.load();
    if (ImGui::BeginCombo("Loop Mode", kLoopModes[loopIdx])) {
        for (int i = 0; i < 5; ++i) {
            bool sel = (i == loopIdx);
            if (ImGui::Selectable(kLoopModes[i], sel)) {
                params.loopModeIndex = i;
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(200);
    int polyphony = params.polyphony.load();
    bool polyChanged = ImGui::SliderInt("Polyphony", &polyphony, 1, 1024);
    polyChanged |= sliderExtrasInt("##polyphonypopup", &polyphony, 256, 1, 1024);
    if (polyChanged) {
        params.polyphony = polyphony;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int notePolyphony = params.notePolyphony.load();
    bool notePolyChanged = ImGui::SliderInt("Note Polyphony", &notePolyphony, 1, 1024);
    notePolyChanged |= sliderExtrasInt("##notepolyphonypopup", &notePolyphony, 256, 1, 1024);
    if (notePolyChanged) {
        params.notePolyphony = notePolyphony;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int sampleVolume = params.volume.load();
    bool sampleVolChanged = ImGui::SliderInt("Volume", &sampleVolume, -48, 48);
    sampleVolChanged |= sliderExtrasInt("##volumepopup", &sampleVolume, 0, -48, 48);
    if (sampleVolChanged) {
        params.volume = sampleVolume;
        if (onParamChanged) onParamChanged();
    }

    ImGui::Spacing();

    ImGui::SetNextItemWidth(160);
    int vol = params.ccVolume.load();
    bool volChanged = ImGui::SliderInt("Volume (CC7)", &vol, 0, 127);
    volChanged |= sliderExtrasInt("##ccvolumepopup", &vol, 100, 0, 127);
    if (volChanged) {
        params.ccVolume = vol;
        params.pendingCcVolume = true;
    }

    ImGui::SetNextItemWidth(160);
    int pan = params.ccPan.load();
    bool panChanged = ImGui::SliderInt("Pan (CC10)", &pan, 0, 127);
    panChanged |= sliderExtrasInt("##ccpanpopup", &pan, 64, 0, 127);
    if (panChanged) {
        params.ccPan = pan;
        params.pendingCcPan = true;
    }

    ImGui::Spacing();

    // Sample tab's "stack" (see shared.hpp's GuiState::StackItem): the
    // file(s) most recently loaded together in one pick/drop - all of them
    // play together as a layer (plugin.cpp's loadStack/regenerateAndLoadSfz).
    // Only path/isSfz/regionCount are copied out here (not regionsText,
    // which can be sizeable for a big multisample mapping and isn't needed
    // for display) to keep this per-frame snapshot cheap.
    struct StackDisplayEntry { std::string path; bool isSfz; int regionCount; };
    std::vector<StackDisplayEntry> stackView;
    std::string stackError, lastBrowseDir;
    int64_t numFrames = 0, loopStartFrame = -1, loopEndFrame = -1;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        stackView.reserve(params.guiState.stack.size());
        for (const auto& item : params.guiState.stack)
            stackView.push_back({item.path, item.isSfz, item.regionCount});
        stackError = params.guiState.stackError;
        lastBrowseDir = params.guiState.lastBrowseDir;
        numFrames = params.guiState.numFrames;
        loopStartFrame = params.guiState.loopStartFrame;
        loopEndFrame = params.guiState.loopEndFrame;
    }
    bool stackHasSfz = false;
    for (const auto& e : stackView)
        if (e.isSfz) { stackHasSfz = true; break; }

    // "Character": -12..12 semitone shift over the whole imported mapping
    // (writes note_offset=/transpose=, see buildSfzText) - only meaningful
    // once the stack contains a real multisample mapping, so only
    // interactive then; the value itself isn't reset when the stack changes.
    if (!stackHasSfz) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(200);
    int character = params.character.load();
    bool characterChanged = ImGui::SliderInt("Character", &character, -12, 12);
    characterChanged |= sliderExtrasInt("##characterpopup", &character, 0, -12, 12);
    if (characterChanged) {
        params.character = character;
        if (onParamChanged) onParamChanged();
    }
    if (!stackHasSfz) ImGui::EndDisabled();

    bool offsetEnabled = params.offsetEnabled.load();
    if (ImGui::Checkbox("Enable Offset", &offsetEnabled)) {
        params.offsetEnabled = offsetEnabled;
        if (onParamChanged) onParamChanged();
    }
    // Only a lone plain sample (nothing stacked on top of it, no SFZ) has a
    // real frame count to size the offset against - any SFZ or 2+ item
    // stack falls back to the fixed default range (see loadStack).
    const int maxOffset = static_cast<int>(
        numFrames > 0 ? numFrames : SampleInfo::defaultMaxOffset);

    // Not gated on offsetEnabled - whatever value is here gets picked up
    // when the checkbox is on, same "don't gate speculatively" convention
    // as the rest of this UI (see project feedback memory).
    int offsetVal = params.offsetValue.load();
    ImGui::SetNextItemWidth(-1);
    bool offsetChanged = ImGui::SliderInt("##offset", &offsetVal, 0, maxOffset, "Offset: %d");
    offsetChanged |= sliderExtrasInt("##offsetpopup", &offsetVal, 0, 0, maxOffset);
    if (offsetChanged) {
        params.offsetValue = offsetVal;
        if (onParamChanged) onParamChanged();
    }

    bool randomOffsetEnabled = params.randomOffsetEnabled.load();
    if (ImGui::Checkbox("Random", &randomOffsetEnabled)) {
        params.randomOffsetEnabled = randomOffsetEnabled;
        if (onParamChanged) onParamChanged();
    }
    // Not independently settable - always max(0, maxOffset - offsetVal),
    // see shared.hpp/regenerateAndLoadSfz. Shown as a disabled slider (for
    // visual consistency with the rest of the panel) rather than plain
    // text, purely to communicate its range at a glance.
    int randomOffsetVal = std::max(0, maxOffset - offsetVal);
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled();
    ImGui::SliderInt("##randomoffset", &randomOffsetVal, 0, maxOffset, "Random Offset: %d");
    ImGui::EndDisabled();

    ImGui::Spacing();

    auto basename = [](const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };

    // A single file picked/dropped always REPLACES whatever's currently
    // loaded (like before the stack feature existed); the stack only forms
    // when the user explicitly selects 2+ files in one go - Ctrl+click
    // several rows in the dialog below, or drop several at once (see
    // plugin.cpp's loadStack, the single entry point both paths share).
    // Loading is never additive across separate picks.
    if (stackView.size() >= 2) {
        // 2+ items stacked: the waveform/single-SFZ views below (each only
        // meaningful for one item) are replaced by a plain list, so the
        // layering is obvious at a glance - see the project spec this
        // implements.
        ImGui::Text("%d items stacked (playing together):", (int)stackView.size());
        if (ImGui::BeginChild("##stacklist", ImVec2(0, 110.f), true)) {
            for (size_t i = 0; i < stackView.size(); ++i) {
                const auto& e = stackView[i];
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("x") && onRemoveStackItem) onRemoveStackItem(i);
                ImGui::SameLine();
                if (e.isSfz)
                    ImGui::Text("%s (SFZ, %d regions)", basename(e.path).c_str(), e.regionCount);
                else
                    ImGui::TextUnformatted(basename(e.path).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        // Named "Replace All" here specifically (not "Load...") since a
        // single pick from this button would otherwise read as "adding" to
        // the visible list, when it actually discards it - see the comment
        // above this whole block.
        if (ImGui::Button("Replace All..."))
            ui.fileDialog.open(FileDialog::OpenFile, "Load Sample or SFZ",
                               loadDialogExtensions(), "", lastBrowseDir);
        ImGui::SameLine();
        if (ImGui::Button("Clear All") && onClearStack) onClearStack();
    } else if (stackView.size() == 1 && stackView[0].isSfz) {
        ImGui::Text("%s (%d regions)", basename(stackView[0].path).c_str(),
                   stackView[0].regionCount);
        if (ImGui::Button("Load Sample or SFZ..."))
            ui.fileDialog.open(FileDialog::OpenFile, "Load Sample or SFZ",
                               loadDialogExtensions(), "", lastBrowseDir);
    } else {
        bool clicked = ui.waveform.draw(120.f, params.offsetEnabled.load() ? offsetVal : -1,
                                        loopStartFrame, loopEndFrame);
        if (clicked)
            ui.fileDialog.open(FileDialog::OpenFile, "Load Sample or SFZ",
                               loadDialogExtensions(), "", lastBrowseDir);
    }

    if (!stackError.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", stackError.c_str());
}

// ----------------------------------------------------------------- pan tab

void drawPanTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    // Doubles the pan range for a plain MIDI CC10 pan message (writes
    // pan_oncc10=200) and doubles every other pan-opcode value this tab (and
    // the FX tab's Opcode FX) writes - for instruments built from hard-
    // panned mono samples where the normal pan range can't bring a channel
    // back across center. See buildSfzText's panMul.
    bool panX2 = params.panX2.load();
    if (ImGui::Checkbox("x2", &panX2)) {
        params.panX2 = panX2;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int panRandom = params.panRandom.load();
    bool prChanged = ImGui::SliderInt("Pan Random", &panRandom, -200, 200);
    prChanged |= sliderExtrasInt("##panrandompopup", &panRandom, 0, -200, 200);
    if (prChanged) {
        params.panRandom = panRandom;
        if (onParamChanged) onParamChanged();
    }

    // Writes pan_oncc136= always (Pan Random's own opcode) plus a second,
    // same-valued opcode that toggles pan_oncc135=/pan_oncc137= - see
    // buildSfzText.
    bool panAlternate = params.panAlternate.load();
    if (ImGui::Checkbox("Alternate", &panAlternate)) {
        params.panAlternate = panAlternate;
        if (onParamChanged) onParamChanged();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("LFO (lfo01_*)");
    ImGui::BeginChild("##panlfo", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    atomicVFader(params.panLfoDelay, "LFO Delay", "##panlfodelay", 0.0f, 4.0f, 0.0f, onParamChanged);
    atomicVFader(params.panLfoFade, "LFO Fade", "##panlfofade", 0.0f, 4.0f, 0.0f, onParamChanged);
    intVFader(params.panLfoPan, "LFO Pan", "##panlfopan", -200, 200, 0, onParamChanged);
    atomicVFader(params.panLfoFreq, "LFO Freq", "##panlfofreq", -20.0f, 20.0f, 10.0f, onParamChanged);
    lfoWaveFader(params.panLfoWave, onParamChanged);
    ImGui::EndChild();
}

// ----------------------------------------------------------------- amp tab

void drawAmpTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    ImGui::SetNextItemWidth(200);
    int ampKeycenter = params.ampKeycenter.load();
    bool akcChanged = ImGui::SliderInt("Amp Keycenter", &ampKeycenter, 0, 127);
    akcChanged |= sliderExtrasInt("##ampkeycenterpopup", &ampKeycenter, 60, 0, 127);
    if (akcChanged) {
        params.ampKeycenter = ampKeycenter;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int ampKeytrack = params.ampKeytrack.load();
    bool aktChanged = ImGui::SliderInt("Amp Keytrack", &ampKeytrack, -96, 12);
    aktChanged |= sliderExtrasInt("##ampkeytrackpopup", &ampKeytrack, 0, -96, 12);
    if (aktChanged) {
        params.ampKeytrack = ampKeytrack;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int ampVeltrack = params.ampVeltrack.load();
    bool avtChanged = ImGui::SliderInt("Amp Veltrack", &ampVeltrack, -100, 100);
    avtChanged |= sliderExtrasInt("##ampveltrackpopup", &ampVeltrack, 95, -100, 100);
    if (avtChanged) {
        params.ampVeltrack = ampVeltrack;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int ampRandom = params.ampRandom.load();
    bool arChanged = ImGui::SliderInt("Amp Random", &ampRandom, -24, 24);
    arChanged |= sliderExtrasInt("##amprandompopup", &ampRandom, 0, -24, 24);
    if (arChanged) {
        params.ampRandom = ampRandom;
        if (onParamChanged) onParamChanged();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Envelope (eg01_*)");
    ImGui::BeginChild("##ampeg", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    atomicVFader(params.ampStartLevel, "Start Level", "##startlevel", 0.0f, 1.0f, 0.0f, onParamChanged);
    atomicVFader(params.ampDelayTime, "Delay Time", "##delaytime", 0.00001f, 4.0f, 0.00001f, onParamChanged);
    atomicVFader(params.ampAttackTime, "Attack Time", "##attacktime", 0.00001f, 8.0f, 0.00001f, onParamChanged);
    atomicVFader(params.ampHoldTime, "Hold Time", "##holdtime", 0.00001f, 8.0f, 0.00001f, onParamChanged);
    atomicVFader(params.ampDecayTime, "Decay Time", "##decaytime", 0.00001f, 20.0f, 0.00001f, onParamChanged);
    atomicVFader(params.ampSustainLevel, "Sustain Level", "##sustainlevel", 0.0f, 1.0f, 1.0f, onParamChanged);
    atomicVFader(params.ampReleaseTime, "Release Time", "##releasetime", 0.00001f, 8.0f, 0.00001f, onParamChanged);
    ImGui::EndChild();

    // Curve ("shape") sliders: horizontal, separate from the vertical
    // Level/Time faders above so they don't get lost among them.
    atomicHSliderFloat(params.ampAttackShape, "Attack Shape", "##attackshapepopup", -11.0f, 11.0f,
                       0.00001f, onParamChanged);
    atomicHSliderFloat(params.ampDecayShape, "Decay Shape", "##decayshapepopup", -11.0f, 11.0f,
                       -0.3616f, onParamChanged);
    atomicHSliderFloat(params.ampReleaseShape, "Release Shape", "##releaseshapepopup", -11.0f, 11.0f,
                       -6.3616f, onParamChanged);

    ImGui::Spacing();
    ImGui::TextDisabled("LFO (lfo01_*)");
    ImGui::BeginChild("##amplfo", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    atomicVFader(params.ampLfoDelay, "LFO Delay", "##lfodelay", 0.0f, 4.0f, 0.0f, onParamChanged);
    atomicVFader(params.ampLfoFade, "LFO Fade", "##lfofade", 0.0f, 4.0f, 0.0f, onParamChanged);
    atomicVFader(params.ampLfoVolume, "LFO Volume", "##lfovolume", -20.0f, 20.0f, 0.0f, onParamChanged);
    atomicVFader(params.ampLfoFreq, "LFO Freq", "##lfofreq", -20.0f, 20.0f, 10.0f, onParamChanged);
    lfoWaveFader(params.ampLfoWave, onParamChanged);
    ImGui::EndChild();
}

// ----------------------------------------------------------------- filter tab

void drawFilterTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    int typeIdx = params.filterTypeIndex.load();
    if (ImGui::BeginCombo("Filter Type", kFilterTypes[typeIdx])) {
        for (int i = 0; i < 23; ++i) {
            bool sel = (i == typeIdx);
            if (ImGui::Selectable(kFilterTypes[i], sel)) {
                params.filterTypeIndex = i;
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(200);
    int cutoff = params.filterCutoff.load();
    bool cutoffChanged = ImGui::SliderInt("Cutoff", &cutoff, 1, 20000);
    cutoffChanged |= sliderExtrasInt("##cutoffpopup", &cutoff, 20000, 1, 20000);
    if (cutoffChanged) {
        params.filterCutoff = cutoff;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    float resonance = params.filterResonance.load();
    bool resoChanged = ImGui::SliderFloat("Resonance (dB)", &resonance, -40.0f, 40.0f, "%.2f");
    resoChanged |= sliderExtrasFloat("##resonancepopup", &resonance, 0.0f, -40.0f, 40.0f);
    if (resoChanged) {
        params.filterResonance = resonance;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int randomCutoff = params.filterRandomCutoff.load();
    bool rcChanged = ImGui::SliderInt("Random Cutoff", &randomCutoff, 1, 20000);
    rcChanged |= sliderExtrasInt("##randomcutoffpopup", &randomCutoff, 0, 1, 20000);
    if (rcChanged) {
        params.filterRandomCutoff = randomCutoff;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int filKeycenter = params.filKeycenter.load();
    bool fkcChanged = ImGui::SliderInt("Fil Keycenter", &filKeycenter, 0, 127);
    fkcChanged |= sliderExtrasInt("##filkeycenterpopup", &filKeycenter, 60, 0, 127);
    if (fkcChanged) {
        params.filKeycenter = filKeycenter;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int filKeytrack = params.filKeytrack.load();
    bool fktChanged = ImGui::SliderInt("Fil Keytrack", &filKeytrack, 0, 1200);
    fktChanged |= sliderExtrasInt("##filkeytrackpopup", &filKeytrack, 0, 0, 1200);
    if (fktChanged) {
        params.filKeytrack = filKeytrack;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int filVeltrack = params.filVeltrack.load();
    bool fvtChanged = ImGui::SliderInt("Fil Veltrack", &filVeltrack, 0, 20000);
    fvtChanged |= sliderExtrasInt("##filveltrackpopup", &filVeltrack, 0, 0, 20000);
    if (fvtChanged) {
        params.filVeltrack = filVeltrack;
        if (onParamChanged) onParamChanged();
    }
    // Opcode name (not value) depends on filterEgEnabled - see buildSfzText.
    ImGui::SameLine();
    ImGui::TextDisabled(params.filterEgEnabled.load() ? "(eg02_cutoff_oncc131)" : "(cutoff_oncc131)");

    ImGui::SetNextItemWidth(200);
    int resoVeltrack = params.resoVeltrack.load();
    bool rvtChanged = ImGui::SliderInt("Reso Veltrack", &resoVeltrack, -40, 40);
    rvtChanged |= sliderExtrasInt("##resoveltrackpopup", &resoVeltrack, 0, -40, 40);
    if (rvtChanged) {
        params.resoVeltrack = resoVeltrack;
        if (onParamChanged) onParamChanged();
    }

    ImGui::Spacing();
    bool filterEgEnabled = params.filterEgEnabled.load();
    if (ImGui::Checkbox("Filter EG", &filterEgEnabled)) {
        params.filterEgEnabled = filterEgEnabled;
        if (onParamChanged) onParamChanged();
    }
    // Not gated on filterEgEnabled - same "don't gate speculatively"
    // convention as the rest of this UI. Whatever's here gets picked up
    // when the checkbox is on.
    ImGui::TextDisabled("Envelope (eg02_*)");
    ImGui::BeginChild("##filtereg", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    intVFader(params.filDepth, "Fil Depth", "##fildepth", 0, 20000, 0, onParamChanged);
    atomicVFader(params.filEgStartLevel, "Start Level", "##filstartlevel", 0.0f, 1.0f, 0.0f, onParamChanged);
    atomicVFader(params.filEgDelayTime, "Delay Time", "##fildelaytime", 0.00001f, 4.0f, 0.00001f, onParamChanged);
    atomicVFader(params.filEgAttackTime, "Attack Time", "##filattacktime", 0.00001f, 8.0f, 0.00001f, onParamChanged);
    atomicVFader(params.filEgHoldTime, "Hold Time", "##filholdtime", 0.00001f, 8.0f, 0.00001f, onParamChanged);
    atomicVFader(params.filEgDecayTime, "Decay Time", "##fildecaytime", 0.00001f, 20.0f, 0.00001f, onParamChanged);
    atomicVFader(params.filEgSustainLevel, "Sustain Level", "##filsustainlevel", 0.0f, 1.0f, 1.0f, onParamChanged);
    atomicVFader(params.filEgReleaseTime, "Release Time", "##filreleasetime", 0.00001f, 8.0f, 0.00001f, onParamChanged);
    ImGui::EndChild();

    // Curve ("shape") sliders: horizontal, separate from the vertical
    // Level/Time faders above so they don't get lost among them.
    atomicHSliderFloat(params.filEgAttackShape, "Attack Shape", "##filattackshapepopup", -11.0f,
                       11.0f, 0.00001f, onParamChanged);
    atomicHSliderFloat(params.filEgDecayShape, "Decay Shape", "##fildecayshapepopup", -11.0f, 11.0f,
                       0.00001f, onParamChanged);
    atomicHSliderFloat(params.filEgReleaseShape, "Release Shape", "##filreleaseshapepopup", -11.0f,
                       11.0f, 0.00001f, onParamChanged);

    ImGui::Spacing();
    ImGui::TextDisabled("LFO (lfo02_*)");
    ImGui::BeginChild("##filterlfo", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    atomicVFader(params.filterLfoDelay, "LFO Delay", "##fillfodelay", 0.0f, 4.0f, 0.0f, onParamChanged);
    atomicVFader(params.filterLfoFade, "LFO Fade", "##fillfofade", 0.0f, 4.0f, 0.0f, onParamChanged);
    intVFader(params.filterLfoDepth, "LFO Depth", "##fillfodepth", -20000, 20000, 0, onParamChanged);
    atomicVFader(params.filterLfoFreq, "LFO Freq", "##fillfofreq", -20.0f, 20.0f, 10.0f, onParamChanged);
    lfoWaveFader(params.filterLfoWave, onParamChanged);
    ImGui::EndChild();
}

// ----------------------------------------------------------------- pitch tab

void drawPitchTab(SharedParams& params, const std::function<void()>& onParamChanged,
                  const std::function<void()>& onLoadScalaFile,
                  const std::function<void()>& onClearScala) {
    ImGui::SetNextItemWidth(200);
    int pitchKeytrack = params.pitchKeytrack.load();
    bool pktChanged = ImGui::SliderInt("Pitch Keytrack", &pitchKeytrack, -1200, 1200);
    pktChanged |= sliderExtrasInt("##pitchkeytrackpopup", &pitchKeytrack, 100, -1200, 1200);
    if (pktChanged) {
        params.pitchKeytrack = pitchKeytrack;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int pitchVeltrack = params.pitchVeltrack.load();
    bool pvtChanged = ImGui::SliderInt("Pitch Veltrack", &pitchVeltrack, -9600, 9600);
    pvtChanged |= sliderExtrasInt("##pitchveltrackpopup", &pitchVeltrack, 0, -9600, 9600);
    if (pvtChanged) {
        params.pitchVeltrack = pitchVeltrack;
        if (onParamChanged) onParamChanged();
    }

    ImGui::SetNextItemWidth(200);
    int pitchRandom = params.pitchRandom.load();
    bool prChanged = ImGui::SliderInt("Pitch Random", &pitchRandom, 0, 9600);
    prChanged |= sliderExtrasInt("##pitchrandompopup", &pitchRandom, 0, 0, 9600);
    if (prChanged) {
        params.pitchRandom = pitchRandom;
        if (onParamChanged) onParamChanged();
    }

    ImGui::Spacing();
    bool portamentoEnabled = params.portamentoEnabled.load();
    if (ImGui::Checkbox("Portamento", &portamentoEnabled)) {
        params.portamentoEnabled = portamentoEnabled;
        if (onParamChanged) onParamChanged();
    }
    // Not gated on portamentoEnabled - same convention as the rest of this
    // UI.
    ImGui::SetNextItemWidth(200);
    float glideTime = params.glideTime.load();
    bool glideChanged = ImGui::SliderFloat("Glide Time", &glideTime, 0.0f, 2.0f, "%.5f");
    glideChanged |= sliderExtrasFloat("##glidetimepopup", &glideTime, 0.0f, 0.0f, 2.0f);
    if (glideChanged) {
        params.glideTime = glideTime;
        if (onParamChanged) onParamChanged();
    }

    ImGui::Spacing();
    bool pitchEgEnabled = params.pitchEgEnabled.load();
    if (ImGui::Checkbox("Pitch EG", &pitchEgEnabled)) {
        params.pitchEgEnabled = pitchEgEnabled;
        if (onParamChanged) onParamChanged();
    }
    // Not gated on pitchEgEnabled - same "don't gate speculatively"
    // convention as the rest of this UI. Whatever's here gets picked up
    // when the checkbox is on.
    ImGui::TextDisabled("Envelope (eg03_*)");
    ImGui::BeginChild("##pitcheg", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    intVFader(params.pitchDepth, "Pitch Depth", "##pitchdepth", -9600, 9600, 0, onParamChanged);
    atomicVFader(params.pitchEgStartLevel, "Start Level", "##pitchstartlevel", 0.0f, 1.0f, 0.0f, onParamChanged);
    atomicVFader(params.pitchEgDelayTime, "Delay Time", "##pitchdelaytime", 0.00001f, 4.0f, 0.00001f, onParamChanged);
    atomicVFader(params.pitchEgAttackTime, "Attack Time", "##pitchattacktime", 0.00001f, 4.0f, 0.00001f, onParamChanged);
    atomicVFader(params.pitchEgHoldTime, "Hold Time", "##pitchholdtime", 0.00001f, 4.0f, 0.00001f, onParamChanged);
    atomicVFader(params.pitchEgDecayTime, "Decay Time", "##pitchdecaytime", 0.00001f, 4.0f, 0.00001f, onParamChanged);
    atomicVFader(params.pitchEgSustainLevel, "Sustain Level", "##pitchsustainlevel", 0.0f, 1.0f, 1.0f, onParamChanged);
    atomicVFader(params.pitchEgReleaseTime, "Release Time", "##pitchreleasetime", 0.00001f, 4.0f, 0.00001f, onParamChanged);
    ImGui::EndChild();

    // Curve ("shape") sliders: horizontal, separate from the vertical
    // Level/Time faders above so they don't get lost among them.
    atomicHSliderFloat(params.pitchEgAttackShape, "Attack Shape", "##pitchattackshapepopup", -11.0f,
                       11.0f, 0.00001f, onParamChanged);
    atomicHSliderFloat(params.pitchEgDecayShape, "Decay Shape", "##pitchdecayshapepopup", -11.0f,
                       11.0f, 0.00001f, onParamChanged);
    atomicHSliderFloat(params.pitchEgReleaseShape, "Release Shape", "##pitchreleaseshapepopup",
                       -11.0f, 11.0f, 0.00001f, onParamChanged);

    ImGui::Spacing();
    ImGui::TextDisabled("LFO (lfo03_*)");
    ImGui::BeginChild("##pitchlfo", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    atomicVFader(params.pitchLfoDelay, "LFO Delay", "##pitchlfodelay", 0.0f, 4.0f, 0.0f, onParamChanged);
    atomicVFader(params.pitchLfoFade, "LFO Fade", "##pitchlfofade", 0.0f, 4.0f, 0.0f, onParamChanged);
    intVFader(params.pitchLfoPitch, "LFO Pitch", "##pitchlfopitch", -2400, 2400, 0, onParamChanged);
    atomicVFader(params.pitchLfoFreq, "LFO Freq", "##pitchlfofreq", -20.0f, 20.0f, 4.5f, onParamChanged);
    lfoWaveFader(params.pitchLfoWave, onParamChanged);
    ImGui::EndChild();

    // "Tuning" section: Scala (.scl) microtonal mapping + the concert-pitch
    // A4 reference - engine-level settings (sfizz_load_scala_file/
    // sfizz_set_tuning_frequency), not SFZ opcodes, so neither control here
    // calls onParamChanged/triggers an SFZ regenerate. Also DAW-session-only
    // (not part of .sspreset/.ssprofile) - see SharedParams::tuningFrequency.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Tuning");

    std::string scalaFilePath, scalaError;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        scalaFilePath = params.guiState.scalaFilePath;
        scalaError = params.guiState.scalaError;
    }
    if (scalaFilePath.empty()) {
        ImGui::TextUnformatted("Standard tuning (12-TET)");
    } else {
        const size_t slash = scalaFilePath.find_last_of('/');
        const std::string base =
            slash == std::string::npos ? scalaFilePath : scalaFilePath.substr(slash + 1);
        ImGui::Text("Scala: %s", base.c_str());
    }
    if (ImGui::Button("Load Scala File...") && onLoadScalaFile) onLoadScalaFile();
    if (!scalaFilePath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear") && onClearScala) onClearScala();
    }
    if (!scalaError.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", scalaError.c_str());

    ImGui::SetNextItemWidth(200);
    float tuningFreq = params.tuningFrequency.load();
    bool tuningChanged =
        ImGui::SliderFloat("Tuning Frequency (Hz)", &tuningFreq, 400.0f, 480.0f, "%.2f");
    tuningChanged |= sliderExtrasFloat("##tuningfreqpopup", &tuningFreq, 440.0f, 400.0f, 480.0f);
    if (tuningChanged) params.tuningFrequency = tuningFreq;
}

// -------------------------------------------------------------- opcodes tab

// Splices out [b,e) (any existing selection, order-independent) and inserts
// insertText at that point, then flags the widget to resync from the
// now-external-edited buffer with the cursor collapsed right after the
// insertion. This is ImGui's documented pattern for mutating an active
// InputText's buffer from outside the widget call (see the comment on
// ImGuiInputTextState::ReloadUserBufAndKeepSelection in imgui_internal.h) -
// we set the Reload* fields directly rather than using that helper because
// it derives its target selection from the state *before* our edit, which
// would restore the now-stale pre-edit range.
void spliceAndReloadCursor(std::string* text, ImGuiInputTextState* state, int b,
                           int e, const char* insertText = "") {
    if (b > e) std::swap(b, e);
    text->erase(b, e - b);
    text->insert(b, insertText);
    state->WantReloadUserBuf = true;
    state->ReloadSelectionStart = state->ReloadSelectionEnd = b + (int)strlen(insertText);
}

// Free-form text editor: whatever the user types here is written verbatim
// into the generated SFZ, after every other <global> opcode (see
// buildSfzText). Edits guiState.customOpcodesText directly under its mutex -
// the mutex must be released before onParamChanged() runs, since
// regenerateAndLoadSfz() takes the same (non-recursive) lock to read it.
//
// Ctrl+X/C/V/A and Delete already work via ImGui's own InputText handling
// (and, since gui_window.cpp now wires up a real X11 CLIPBOARD backend, they
// round-trip with the OS clipboard). This adds the mouse-driven equivalent
// every text editor has: a right-click context menu, its entries picked
// with a normal left click, reaching into ImGui's internal input-text state
// (GetInputTextState) so it operates on the live selection/cursor exactly
// like the keyboard shortcuts do.
void drawOpcodesTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    ImGui::TextDisabled("Custom opcodes (written after everything else in <global>):");
    bool changed;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        std::string& text = params.guiState.customOpcodesText;
        changed = inputTextMultilineStdString("##customopcodes", &text, ImVec2(-1, -1));

        // Only non-null once the field has actually been focused at least
        // once (ImGui tracks a single active InputText at a time). Cut/Copy/
        // Delete/Select All need it; Paste falls back to appending when the
        // field has never been focused, since there's no cursor to speak of.
        ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID());
        bool hasSelection = state && state->HasSelection();

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Cortar", "Ctrl+X", false, hasSelection)) {
                int b = state->GetSelectionStart(), e = state->GetSelectionEnd();
                ImGui::SetClipboardText(
                    text.substr(std::min(b, e), std::abs(e - b)).c_str());
                spliceAndReloadCursor(&text, state, b, e);
                changed = true;
            }
            if (ImGui::MenuItem("Copiar", "Ctrl+C", false, hasSelection)) {
                int b = state->GetSelectionStart(), e = state->GetSelectionEnd();
                ImGui::SetClipboardText(
                    text.substr(std::min(b, e), std::abs(e - b)).c_str());
            }
            if (ImGui::MenuItem("Pegar", "Ctrl+V")) {
                if (const char* clip = ImGui::GetClipboardText()) {
                    if (state) {
                        int b = state->HasSelection() ? state->GetSelectionStart()
                                                      : state->GetCursorPos();
                        int e = state->HasSelection() ? state->GetSelectionEnd()
                                                      : state->GetCursorPos();
                        spliceAndReloadCursor(&text, state, b, e, clip);
                    } else {
                        text.append(clip);
                    }
                    changed = true;
                }
            }
            if (ImGui::MenuItem("Borrar", "Supr", false, hasSelection)) {
                spliceAndReloadCursor(&text, state, state->GetSelectionStart(),
                                      state->GetSelectionEnd());
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Seleccionar todo", "Ctrl+A", false, state != nullptr))
                state->SelectAll();
            ImGui::EndPopup();
        }
    }
    if (changed && onParamChanged) onParamChanged();
}

// ------------------------------------------------------------------- fx tab

// "CC MODE" combobox shared by the FX tab's Detune/Delay/FX Phase sliders -
// index 0 ("None") uses that opcode's own dedicated default CC (set up in
// the generated <control> block, see buildSfzText), 1-3 redirect to the
// shared cc135/136/137 slots used elsewhere in this UI (Pan/Amp/Filter
// Random).
void ccModeCombo(const char* label, std::atomic<int>& param,
                 const std::function<void()>& onParamChanged) {
    int idx = param.load();
    ImGui::SetNextItemWidth(90);
    if (ImGui::BeginCombo(label, kFxCcModeNames[idx])) {
        for (int i = 0; i < 4; ++i) {
            bool sel = (i == idx);
            if (ImGui::Selectable(kFxCcModeNames[i], sel)) {
                param = i;
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// "Opcode FX": repeats the loaded sample into 2-3 <group>s (unison/chorus)
// instead of a single <region> - see buildSfzText for the exact per-mode
// opcode layout. Every control here stays live/editable regardless of
// fxEnabled/fxMode (same "don't gate speculatively" convention as the rest
// of this UI, e.g. the Filter/Pitch EG sections) - only buildSfzText decides
// what actually ends up written.
void drawFxTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    ImGui::TextDisabled("Opcode");
    bool fxEnabled = params.fxEnabled.load();
    if (ImGui::Checkbox("Opcode FX", &fxEnabled)) {
        params.fxEnabled = fxEnabled;
        if (onParamChanged) onParamChanged();
    }

    int fxMode = params.fxMode.load();
    if (ImGui::BeginCombo("Mode", kSampleFxModeNames[fxMode])) {
        for (int i = 0; i < 4; ++i) {
            bool sel = (i == fxMode);
            if (ImGui::Selectable(kSampleFxModeNames[i], sel)) {
                params.fxMode = i;
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    ImGui::SetNextItemWidth(160);
    int detune = params.fxDetune.load();
    bool detuneChanged = ImGui::SliderInt("Detune", &detune, -100, 100);
    detuneChanged |= sliderExtrasInt("##fxdetunepopup", &detune, 15, -100, 100);
    if (detuneChanged) {
        params.fxDetune = detune;
        if (onParamChanged) onParamChanged();
    }
    ImGui::SameLine();
    ccModeCombo("##fxdetunecc", params.fxDetuneCcMode, onParamChanged);

    ImGui::SetNextItemWidth(160);
    float delay = params.fxDelay.load();
    bool delayChanged = ImGui::SliderFloat("Delay", &delay, 0.0f, 0.1f, "%.5f");
    delayChanged |= sliderExtrasFloat("##fxdelaypopup", &delay, 0.05f, 0.0f, 0.1f);
    if (delayChanged) {
        params.fxDelay = delay;
        if (onParamChanged) onParamChanged();
    }
    ImGui::SameLine();
    ccModeCombo("##fxdelaycc", params.fxDelayCcMode, onParamChanged);

    ImGui::SetNextItemWidth(160);
    int stereoWidth = params.fxStereoWidth.load();
    bool widthChanged = ImGui::SliderInt("Stereo Width", &stereoWidth, 0, 100);
    widthChanged |= sliderExtrasInt("##fxwidthpopup", &stereoWidth, 0, 0, 100);
    if (widthChanged) {
        params.fxStereoWidth = stereoWidth;
        if (onParamChanged) onParamChanged();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Unison mode only)");

    ImGui::Spacing();
    ImGui::TextDisabled("Chorus (lfo05_*, every mode except Unison)");
    bool independentLfo = params.fxIndependentLfo.load();
    if (ImGui::Checkbox("Independent LFO", &independentLfo)) {
        params.fxIndependentLfo = independentLfo;
        if (onParamChanged) onParamChanged();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Chorus Stereo modes only)");
    ImGui::BeginChild("##fxchorus", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar);
    intVFader(params.fxDepth, "FX Depth", "##fxdepth", -100, 100, -15, onParamChanged);
    atomicVFader(params.fxSpeed, "FX Speed", "##fxspeed", 0.0f, 10.0f, 0.5f, onParamChanged);
    lfoWaveFader(params.fxWave, onParamChanged, 0);
    ImGui::EndChild();

    ImGui::SetNextItemWidth(160);
    float fxPhase = params.fxPhase.load();
    bool phaseChanged = ImGui::SliderFloat("FX Phase", &fxPhase, 0.0f, 1.0f, "%.5f");
    phaseChanged |= sliderExtrasFloat("##fxphasepopup", &fxPhase, 0.5f, 0.0f, 1.0f);
    if (phaseChanged) {
        params.fxPhase = fxPhase;
        if (onParamChanged) onParamChanged();
    }
    ImGui::SameLine();
    ccModeCombo("##fxphasecc", params.fxPhaseCcMode, onParamChanged);

    // Second filter (fil2type=/cutoff2=) + makeup gain (volume=, see
    // fxVolume below): only take effect on the wet oscillator(s) - Chorus
    // Mono's oscillator 1, and both oscillators of Chorus Stereo (Wet+Dry)
    // - see buildSfzText for exactly where they're written; harmless
    // (unused opcodes) in Unison/Chorus Stereo (Wet).
    ImGui::Spacing();
    ImGui::TextDisabled("2nd Filter (wet osc. only: Chorus Mono, Chorus Stereo Wet+Dry)");
    int fil2TypeIdx = params.fil2TypeIndex.load();
    if (ImGui::BeginCombo("Filter 2 Type", kFilterTypes[fil2TypeIdx])) {
        for (int i = 0; i < 23; ++i) {
            bool sel = (i == fil2TypeIdx);
            if (ImGui::Selectable(kFilterTypes[i], sel)) {
                params.fil2TypeIndex = i;
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(200);
    int cutoff2 = params.cutoff2.load();
    bool cutoff2Changed = ImGui::SliderInt("Cutoff 2", &cutoff2, 1, 20000);
    cutoff2Changed |= sliderExtrasInt("##cutoff2popup", &cutoff2, 11700, 1, 20000);
    if (cutoff2Changed) {
        params.cutoff2 = cutoff2;
        if (onParamChanged) onParamChanged();
    }

    // Makeup gain: volume=sample_volume+fxVolume (see buildSfzText, both
    // are dB values that stack) - balances the wet oscillator(s)' loudness
    // against the Sample tab's plain volume=.
    ImGui::SetNextItemWidth(200);
    int fxVolume = params.fxVolume.load();
    bool fxVolumeChanged = ImGui::SliderInt("Volume", &fxVolume, -48, 0);
    fxVolumeChanged |= sliderExtrasInt("##fxvolumepopup", &fxVolume, -6, -48, 0);
    if (fxVolumeChanged) {
        params.fxVolume = fxVolume;
        if (onParamChanged) onParamChanged();
    }

    // Real FX: sfizz's built-in fverb reverb (<effect> type=fverb, see
    // buildSfzText) - unrelated to the "Opcode FX" section above, which
    // emulates chorus/unison via <group>/<region> repetition instead of a
    // real DSP effect. reverbEnabled (default off) gates whether the
    // <effect> block is written at all; the controls below it stay
    // live/editable regardless, same "don't gate speculatively" convention
    // as "Opcode FX" above - only buildSfzText decides what's written.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Reverb (real, <effect> type=fverb)");
    bool reverbEnabled = params.reverbEnabled.load();
    if (ImGui::Checkbox("DSP FX", &reverbEnabled)) {
        params.reverbEnabled = reverbEnabled;
        if (onParamChanged) onParamChanged();
    }

    int reverbTypeIdx = params.reverbTypeIndex.load();
    if (ImGui::BeginCombo("Reverb Type", kReverbTypeNames[reverbTypeIdx])) {
        for (int i = 0; i < 7; ++i) {
            bool sel = (i == reverbTypeIdx);
            if (ImGui::Selectable(kReverbTypeNames[i], sel)) {
                params.reverbTypeIndex = i;
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    auto reverbSlider = [&](const char* label, const char* popupId,
                            std::atomic<float>& param, float defaultValue) {
        ImGui::SetNextItemWidth(160);
        float v = param.load();
        bool changed = ImGui::SliderFloat(label, &v, 0.0f, 100.0f, "%.2f");
        changed |= sliderExtrasFloat(popupId, &v, defaultValue, 0.0f, 100.0f);
        if (changed) {
            param = v;
            if (onParamChanged) onParamChanged();
        }
    };
    reverbSlider("Input", "##reverbinputpopup", params.reverbInput, 100.0f);
    reverbSlider("Predelay", "##reverbpredelaypopup", params.reverbPredelay, 50.0f);
    reverbSlider("Size", "##reverbsizepopup", params.reverbSize, 50.0f);
    reverbSlider("Tone", "##reverbtonepopup", params.reverbTone, 50.0f);
    reverbSlider("Damp", "##reverbdamppopup", params.reverbDamp, 50.0f);
    reverbSlider("Dry", "##reverbdrypopup", params.reverbDry, 100.0f);
    reverbSlider("Wet", "##reverbwetpopup", params.reverbWet, 100.0f);
}

// ----------------------------------------------------------------- log tab

void drawLogTab(SharedParams& params) {
    ImGui::TextDisabled("MIDI monitor (most recent first):");
    ImGui::BeginChild("##midilog", ImVec2(0, 200), true);
    uint32_t total = params.midiLogWriteCount.load();
    uint32_t count = std::min<uint32_t>(total, SharedParams::kMidiLogCapacity);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (total - 1 - i) % SharedParams::kMidiLogCapacity;
        const SharedParams::MidiLogEntry& e = params.midiLog[idx];
        ImGui::Text("%s  note=%-3d vel=%-3d ch=%d", e.isNoteOn ? "ON " : "OFF",
                   e.noteNumber, e.velocity, e.channel);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("Current SFZ text:");
    std::string sfzCopy;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        sfzCopy = params.guiState.lastSfzText;
    }
    ImGui::BeginChild("##sfztext", ImVec2(0, 150), true);
    ImGui::TextUnformatted(sfzCopy.c_str());
    ImGui::EndChild();
}


// ---------------------------------------------------------------- External
//
// MIDI preprocessor (midi/ExternalMidi.h) - nothing here is an SFZ opcode,
// so no control calls onParamChanged; the audio thread reads
// params.external fresh every block. Sliders tagged [CCn] follow that CC
// when the DAW sends it (the audio thread writes the same atomic), and are
// otherwise just the value in use.

void extFloatSlider(std::atomic<float>& param, const char* label, const char* popupId, float lo,
                    float hi, float defaultValue, const char* format) {
    ImGui::SetNextItemWidth(200);
    float v = param.load();
    bool changed = ImGui::SliderFloat(label, &v, lo, hi, format);
    changed |= sliderExtrasFloat(popupId, &v, defaultValue, lo, hi);
    if (changed) param = v;
}

void extCombo(std::atomic<int>& param, const char* label, const char* const* names, int count) {
    ImGui::SetNextItemWidth(200);
    int idx = std::clamp(param.load(), 0, count - 1);
    if (ImGui::BeginCombo(label, names[idx])) {
        for (int i = 0; i < count; ++i) {
            bool sel = i == idx;
            if (ImGui::Selectable(names[i], sel)) param = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void extSlopeControls(std::atomic<int>& mode, std::atomic<float>& slope, const char* modeLabel,
                      const char* slopeLabel, const char* slopePopupId) {
    extCombo(mode, modeLabel, kExtSlopeModeNames, 2);
    // Slope only shapes the Vital power curve; the ExMachina cosine is fixed.
    const bool vital = mode.load() == 0;
    if (!vital) ImGui::BeginDisabled();
    extFloatSlider(slope, slopeLabel, slopePopupId, -8.0f, 8.0f, 0.0f, "%.2f");
    if (!vital) ImGui::EndDisabled();
}

void drawExternalTab(SharedParams& params) {
    ExternalParams& ext = params.external;

    bool enabled = ext.enabled.load();
    if (ImGui::Checkbox("Enable External MIDI preprocessor", &enabled)) ext.enabled = enabled;
    ImGui::TextDisabled("Cents are converted to pitch bend through Bend Range (+%d / %d cents).",
                        params.bendUpCents.load(), params.bendDownCents.load());

    if (!enabled) ImGui::BeginDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText("Pitch randomizer");
    ImGui::TextDisabled("Note-on slide: starts at a random offset, slides back to pitch");
    extFloatSlider(ext.noteOnCents, "Note-on Range (cents)", "##extoncentspopup", 0.0f, 1200.0f,
                   0.0f, "%.1f");
    extCombo(ext.noteOnPolarity, "Note-on Polarity", kExtPolarityNames, 3);
    extFloatSlider(ext.noteOnMinMs, "Note-on Min Length (ms)", "##extonminpopup", 0.0f, 2000.0f,
                   30.0f, "%.1f");
    extFloatSlider(ext.noteOnMaxMs, "Note-on Max Length (ms)", "##extonmaxpopup", 0.0f, 2000.0f,
                   120.0f, "%.1f");
    ImGui::TextDisabled("Note-off slide: drifts to a random offset during the release");
    extFloatSlider(ext.noteOffCents, "Note-off Range (cents)", "##extoffcentspopup", 0.0f,
                   1200.0f, 0.0f, "%.1f");
    extCombo(ext.noteOffPolarity, "Note-off Polarity", kExtPolarityNames, 3);
    extFloatSlider(ext.noteOffMinMs, "Note-off Min Length (ms)", "##extoffminpopup", 0.0f, 2000.0f,
                   50.0f, "%.1f");
    extFloatSlider(ext.noteOffMaxMs, "Note-off Max Length (ms)", "##extoffmaxpopup", 0.0f, 2000.0f,
                   200.0f, "%.1f");
    extCombo(ext.driftModel, "Drift Model", kExtDriftModelNames, 3);
    extFloatSlider(ext.driftScale, "Drift Depth (%)", "##extdriftpopup", 0.0f, 400.0f, 100.0f,
                   "%.0f");

    ImGui::Spacing();
    ImGui::SeparatorText("Mono legato");
    bool mono = ext.mono.load();
    if (ImGui::Checkbox("Mono Legato [CC16]", &mono)) ext.mono = mono;
    ImGui::SameLine();
    bool retrigger = ext.retrigger.load();
    if (ImGui::Checkbox("Retrigger Sample", &retrigger)) ext.retrigger = retrigger;
    ImGui::TextDisabled(retrigger
                            ? "Each legato key retriggers the sample (still mono), gliding from the previous pitch."
                            : "Only the first note reaches sfizz; later keys bend it, never retrigger.");
    if (mono && params.mpeEnabled.load())
        ImGui::TextDisabled("MPE on: each note sfizz plays gets its own channel (2-16), so the\n"
                            "previous note's release keeps its pitch and blends into the next.");
    extFloatSlider(ext.glideMs, "Glide Size (ms) [CC14]", "##extglidepopup", 0.0f, 1500.0f, 0.0f,
                   ext.glideMs.load() <= 0.0f ? "off" : "%.0f");
    extSlopeControls(ext.glideSlopeMode, ext.glideSlope, "Glide Slope Mode", "Glide Slope",
                     "##extglideslopepopup");

    ImGui::Spacing();
    ImGui::SeparatorText("Amplitude expression glide");
    bool ampExpr = ext.ampExprEnabled.load();
    if (ImGui::Checkbox("Amplitude Expression Glide [CC15]", &ampExpr)) ext.ampExprEnabled = ampExpr;
    extFloatSlider(ext.ampExprAmount, "Effect Quantity (%)", "##extampamountpopup", 0.0f, 100.0f,
                   50.0f, "%.0f");
    extSlopeControls(ext.ampExprSlopeMode, ext.ampExprSlope, "Amp Slope Mode", "Amp Slope",
                     "##extampslopepopup");

    ImGui::Spacing();
    ImGui::SeparatorText("Vibrato");
    bool vibPitch = ext.vibratoPitch.load();
    if (ImGui::Checkbox("Vibrato Pitch", &vibPitch)) ext.vibratoPitch = vibPitch;
    ImGui::SameLine();
    {
        ImGui::SetNextItemWidth(110);
        int ccOut = std::clamp(ext.vibratoCcOut.load(), 0, 127);
        char preview[16];
        if (ccOut == 0) std::snprintf(preview, sizeof(preview), "Off");
        else std::snprintf(preview, sizeof(preview), "CC%d", ccOut);
        if (ImGui::BeginCombo("Vibrato -> CC", preview)) {
            for (int i = 0; i <= 127; ++i) {
                char label[16];
                if (i == 0) std::snprintf(label, sizeof(label), "Off");
                else std::snprintf(label, sizeof(label), "CC%d", i);
                bool sel = i == ccOut;
                if (ImGui::Selectable(label, sel)) ext.vibratoCcOut = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ccOut > 0)
            ImGui::TextDisabled("CC%d rests at 64 (0.5); e.g. cutoff_oncc%d=1200 cutoff_curvecc%d=1",
                                ccOut, ccOut, ccOut);
    }
    ImGui::SetNextItemWidth(200);
    int vibAmount = ext.vibratoAmount.load();
    bool vibChanged = ImGui::SliderInt("Vibrato Amount [CC1]", &vibAmount, 0, 127);
    vibChanged |= sliderExtrasInt("##extvibamountpopup", &vibAmount, 0, 0, 127);
    if (vibChanged) ext.vibratoAmount = vibAmount;
    extFloatSlider(ext.vibratoDepthCents, "Max Depth (cents)", "##extvibdepthpopup", 0.0f, 200.0f,
                   35.0f, "%.1f");
    extFloatSlider(ext.vibratoRateHz, "Rate (Hz)", "##extvibratepopup", 0.5f, 12.0f, 5.2f, "%.2f");
    extCombo(ext.vibratoModel, "Vibrato Model", kExtVibratoModelNames, 2);

    if (!enabled) ImGui::EndDisabled();
}

} // namespace

void drawEditorUI(SharedParams& params, EditorUIState& ui,
                  const std::function<void()>& onParamChanged,
                  const std::function<void(const std::vector<std::string>&)>& onLoadRequested,
                  const std::function<void()>& onSavePreset,
                  const std::function<void()>& onLoadPreset,
                  const std::function<void()>& onSaveProfile,
                  const std::function<void()>& onLoadProfile,
                  const std::function<void()>& onExportSfz,
                  const std::function<void()>& onResetToDefault,
                  const std::function<void(size_t)>& onRemoveStackItem,
                  const std::function<void()>& onClearStack,
                  const std::function<void()>& onLoadScalaFile,
                  const std::function<void()>& onClearScala) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    float width = ImGui::GetContentRegionAvail().x;

    // Persistent Preset/Profile row - always visible regardless of which
    // tab is open (same "pinned outside the tab body" convention as the
    // tune+piano bar below the tabs), since these save/restore the WHOLE
    // instrument, not anything tab-specific. Each button owns its own
    // native (zenity) dialog and error handling in plugin.cpp - this just
    // invokes the callback on click.
    if (ImGui::Button("Save Preset") && onSavePreset) onSavePreset();
    ImGui::SameLine();
    if (ImGui::Button("Load Preset") && onLoadPreset) onLoadPreset();
    ImGui::SameLine();
    if (ImGui::Button("Save Profile") && onSaveProfile) onSaveProfile();
    ImGui::SameLine();
    if (ImGui::Button("Load Profile") && onLoadProfile) onLoadProfile();
    ImGui::SameLine();
    if (ImGui::Button("Export SFZ") && onExportSfz) onExportSfz();
    ImGui::SameLine();
    // Destructive (wipes the current instrument design), so it's gated
    // behind a confirmation modal rather than firing straight from the
    // click - see EditorUIState::resetConfirmTrigger's own comment for the
    // deferred-open reason.
    if (ImGui::Button("Reset to Default")) ui.resetConfirmTrigger = true;
    {
        std::string presetError;
        {
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            presetError = params.guiState.presetError;
        }
        if (!presetError.empty())
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", presetError.c_str());
    }

    if (ui.resetConfirmTrigger) {
        ImGui::OpenPopup("Reset to Default?");
        ui.resetConfirmTrigger = false;
    }
    if (ImGui::BeginPopupModal("Reset to Default?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Reset every instrument parameter to its default value?");
        ImGui::TextUnformatted(
            "The loaded sample/SFZ stack, Scala tuning, and tuning frequency stay untouched.");
        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            if (onResetToDefault) onResetToDefault();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();

    // Debug-only, opt-in: SOLOSAMPLER_DEBUG_TAB=<Name> forces that tab open
    // on the first frame, purely so test/mini_host's screenshot technique
    // (no synthetic mouse input in this sandbox) can verify tabs other than
    // the first one. Unset in normal use - no effect on real hosts.
    static const char* forcedTab = getenv("SOLOSAMPLER_DEBUG_TAB");
    auto tabFlags = [](const char* name) {
        return (forcedTab && !strcmp(forcedTab, name)) ? ImGuiTabItemFlags_SetSelected : 0;
    };

    // Reserve fixed space at the bottom for the tune slider + piano (drawn
    // after EndTabBar below, so it stays put regardless of which tab is
    // open) - everything else goes to a per-tab scrollable child, so a tab
    // that outgrows the window scrolls internally (both axes - individual
    // rows of vertical faders can run wide, and the tab as a whole can run
    // tall) instead of pushing the tune/piano row down past the fixed-size
    // root window's clip rect (which, per SetNextWindowSize above, never
    // scrolls or resizes).
    const float pianoAreaHeight =
        ImGui::GetFrameHeightWithSpacing() +                        // tune slider row
        88.f + ImGui::GetStyle().ScrollbarSize + 4.f +               // drawPiano's child (pianoH=88)
        ImGui::GetTextLineHeightWithSpacing() +                     // "Voices: N / M" row
        ImGui::GetStyle().ItemSpacing.y;                             // the Spacing() below

    auto tabBody = [&](const char* childId, const std::function<void()>& body) {
        ImGui::BeginChild(childId, ImVec2(0, ImGui::GetContentRegionAvail().y - pianoAreaHeight),
                          false, ImGuiWindowFlags_HorizontalScrollbar);
        body();
        ImGui::EndChild();
    };

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Sample", nullptr, tabFlags("Sample"))) {
            tabBody("##sampletabscroll", [&] {
                drawSampleTab(params, ui, onParamChanged, onRemoveStackItem, onClearStack);
            });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pan", nullptr, tabFlags("Pan"))) {
            tabBody("##pantabscroll", [&] { drawPanTab(params, onParamChanged); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Amp", nullptr, tabFlags("Amp"))) {
            tabBody("##amptabscroll", [&] { drawAmpTab(params, onParamChanged); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Filter", nullptr, tabFlags("Filter"))) {
            tabBody("##filtertabscroll", [&] { drawFilterTab(params, onParamChanged); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pitch", nullptr, tabFlags("Pitch"))) {
            tabBody("##pitchtabscroll", [&] {
                drawPitchTab(params, onParamChanged, onLoadScalaFile, onClearScala);
            });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Opcodes", nullptr, tabFlags("Opcodes"))) {
            tabBody("##opcodestabscroll", [&] { drawOpcodesTab(params, onParamChanged); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FX", nullptr, tabFlags("FX"))) {
            tabBody("##fxtabscroll", [&] { drawFxTab(params, onParamChanged); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("External", nullptr, tabFlags("External"))) {
            tabBody("##externaltabscroll", [&] { drawExternalTab(params); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log", nullptr, tabFlags("Log"))) {
            tabBody("##logtabscroll", [&] { drawLogTab(params); });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    drawTuneAndPiano(params, ui, width, onParamChanged);

    ImGui::End();

    // Drawn unconditionally (not inside the Sample tab) so it stays open
    // and visible even if the user switches tabs while it's up.
    if (ui.fileDialog.draw() && onLoadRequested)
        onLoadRequested(ui.fileDialog.results());
}
