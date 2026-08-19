// Self-contained ImGui file browser (open and save) -- no external toolkit
// dependencies, works embedded in any host.
//
// Ported verbatim from NeoLooper (sibling project) - SoloSampler only uses
// OpenFile mode (no export/save flow), but the class is generic and
// self-contained either way, so there's nothing to trim.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

class FileDialog {
public:
    enum Mode { OpenFile, SaveFile };

    // startDir: if non-empty and it exists, the browser jumps there instead
    // of reusing wherever it was last left (e.g. the last loaded file's
    // directory, so re-opening the dialog picks up where the user's files
    // actually are rather than wherever they last happened to browse to).
    void open(Mode mode, const char* title,
              std::vector<std::string> extensions, // lowercase: ".wav"...
              const std::string& defaultName = "",
              const std::string& startDir = "");

    // Optional combo in the dialog's footer (meant for SaveFile: e.g. bit
    // depth on export). Configure AFTER open(); cleared on the next open().
    // The choice ends up in optionIndex() once draw() returns true.
    void setOptions(const char* label, std::vector<std::string> items, int index);
    int optionIndex() const { return optIndex_; }

    // Draws the dialog; returns true if the user accepted, leaving the
    // chosen path(s) in results() (result() is just results().front(), kept
    // for SaveFile/single-path callers). In OpenFile mode: Ctrl+click toggles
    // a file in/out of a multi-selection (highlighted like a normal click);
    // double-click always accepts immediately with just that one file,
    // discarding any multi-selection/staged files rather than silently
    // carrying them into the next accept. The "Add" button moves the
    // current selection into a staged list (shown above the buttons, each
    // entry individually removable) that SURVIVES navigating to a different
    // folder - unlike the multi-selection, which is scoped to one directory
    // listing - so files from different folders can be combined into one
    // accept. The Open button accepts staged files plus whatever's currently
    // selected (so the very last folder browsed doesn't need its own "Add"
    // click); with nothing staged and nothing multi-selected, it's just the
    // single last-clicked file, same as before this existed. A caller always
    // gets exactly what the user explicitly assembled, never an accumulation
    // across separate accepts. The dialog stays open after an accept
    // (isOpen() keeps returning true) so the caller can keep reacting to
    // draw()==true for one pick after another - it only closes on
    // Cancel/window-close. SaveFile mode is unchanged (no multi-select/
    // staging there): it closes on accept, same as a normal save dialog.
    bool draw();
    const std::string& result() const { return result_; }
    const std::vector<std::string>& results() const { return results_; }
    bool isOpen() const { return open_; }

private:
    void refresh();
    bool matchesFilter(const std::string& name) const;

    Mode mode_ = OpenFile;
    bool open_ = false;
    bool needRefresh_ = false;
    std::string title_;
    std::vector<std::string> exts_;
    std::string cwd_;
    char pathEdit_[1024] = {};
    char nameEdit_[512] = {};
    struct Entry {
        std::string name;
        bool isDir;
        uint64_t size;
    };
    std::vector<Entry> entries_;
    std::string result_;
    std::vector<std::string> results_;
    // Ctrl+click multi-selection (OpenFile only), by filename within cwd_ -
    // cleared on open()/directory change, and consumed (cleared) on accept.
    std::set<std::string> multiSelected_;
    // "Add"-staged files (OpenFile only), full absolute paths so they
    // survive navigating to a different directory (unlike multiSelected_) -
    // cleared on open() and consumed (cleared) on accept; individual entries
    // removable via the small "x" button drawn next to each one.
    std::vector<std::string> staged_;
    std::string error_;
    // Overwrite confirmation (SaveFile): if the chosen name already exists on
    // disk, confirmation is requested before accepting. overwriteTrigger_ is
    // a one-shot trigger (consumed on the first draw() after being set) that
    // opens the modal popup; pendingPath_ is the path awaiting that
    // confirmation.
    bool overwriteTrigger_ = false;
    std::string pendingPath_;

    std::string optLabel_;
    std::vector<std::string> optItems_;
    int optIndex_ = 0;
};
