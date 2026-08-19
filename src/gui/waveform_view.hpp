// Read-only waveform overview: a cached min/max peak view of the loaded
// sample, plus the offset position and (if present) loop start/end
// markers. No editing/selection/zoom - SoloSampler only needs a visual
// reference and a click target to open the file dialog, unlike NeoLooper's
// fully editable waveform_view.cpp (mouse-wheel zoom, draggable markers,
// envelope overlay) this isn't a port of: porting it would mean stripping
// away most of what it does, so this is written fresh instead.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class WaveformView {
public:
    // Loads and caches peak data for path (GUI thread only - safe to call
    // on every sample load: one linear pass over the file, not per frame).
    // Clears the cache if the file can't be read.
    void load(const std::string& path);
    void clear();
    bool hasData() const { return !peaksMin_.empty(); }

    // Draws into the available width x height. Pass a negative frame number
    // for any marker that shouldn't be drawn (e.g. no loop points).
    // Returns true if the view was left-clicked (caller's cue to open the
    // file dialog).
    bool draw(float height, int64_t offsetFrames, int64_t loopStartFrame,
             int64_t loopEndFrame);

private:
    std::vector<float> peaksMin_, peaksMax_;
    int64_t numFrames_ = 0;
};
