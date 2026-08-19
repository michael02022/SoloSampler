// SoloSampler's ImGui widget layer - draws the whole plugin window each
// frame. Knows nothing about CLAP or sfizz directly: it reads/writes
// SharedParams atomics and calls back into plugin.cpp (onParamChanged)
// whenever a control that feeds the SFZ text changes.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "file_dialog.hpp"
#include "shared.hpp"
#include "waveform_view.hpp"

// GUI-thread-only interactive state (which piano key the mouse is currently
// holding down, the cached waveform peaks, the file browser's open/closed
// state, etc.) - never touched by the audio thread, so it lives outside
// SharedParams.
struct EditorUIState {
    int heldPianoKey = -1;
    bool pianoScrolledOnce = false;
    WaveformView waveform;
    FileDialog fileDialog;
    // One-shot trigger for the "Reset to Default" confirmation modal - set
    // by the button click, consumed (ImGui::OpenPopup + cleared) on the next
    // draw, same deferred-open pattern file_dialog.cpp's own "Replace file"
    // confirmation uses.
    bool resetConfirmTrigger = false;
};

// onParamChanged: call whenever a control that feeds the SFZ text changes.
// onLoadRequested: call with the file(s) the user picked via the file
// dialog in one go - a single pick REPLACES the Sample tab's whole stack
// (like loading always worked before the stack feature), a multi-selection
// (Ctrl+click several rows, see file_dialog.hpp) replaces it with all of
// them, layered together. Never accumulates across separate calls (drag-and-
// drop is handled separately, at the gui_window level, and doesn't go
// through here - both end up in plugin.cpp's loadStack either way, which is
// what actually enforces "replace, not accumulate").
// onSavePreset/onLoadPreset/onSaveProfile/onLoadProfile/onExportSfz: the
// persistent button row's actions (see plugin.cpp's
// handleSavePresetOrProfile/handleLoadPresetOrProfile/handleExportSfz) -
// each one owns its own native (zenity) file dialog and error reporting via
// guiState.presetError, drawEditorUI just invokes the callback on click.
// onResetToDefault: the persistent row's "Reset to Default" button (next to
// Export SFZ) - fires only after the user confirms a modal drawEditorUI
// itself owns (see plugin.cpp's handleResetToDefault for what it actually
// resets: every instrument-design parameter, NOT the loaded sample/SFZ
// stack or Scala tuning/tuning frequency).
// onRemoveStackItem/onClearStack: the Sample tab's stack-list "x"/"Clear
// All" buttons (only shown once 2+ items are stacked - see drawSampleTab).
// onLoadScalaFile/onClearScala: the Pitch tab's "Tuning" section (see
// plugin.cpp's handleLoadScalaFile/handleClearScala) - Load Scala File owns
// its own native (zenity) dialog and error reporting via
// guiState.scalaError; Clear reverts to standard 12-TET.
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
                  const std::function<void()>& onClearScala);
