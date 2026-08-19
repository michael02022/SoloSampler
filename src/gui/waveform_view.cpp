#include "waveform_view.hpp"

#include <sndfile.h>

#include <algorithm>
#include <cstdio>

#include "imgui.h"

namespace {
constexpr int kNumColumns = 1024;
}

void WaveformView::load(const std::string& path) {
    clear();

    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file) return;
    if (info.frames <= 0 || info.channels <= 0) {
        sf_close(file);
        return;
    }

    numFrames_ = info.frames;
    peaksMin_.assign(kNumColumns, 0.f);
    peaksMax_.assign(kNumColumns, 0.f);

    const sf_count_t framesPerColumn =
        std::max<sf_count_t>(1, numFrames_ / kNumColumns);
    std::vector<float> buf(static_cast<size_t>(framesPerColumn) * info.channels);

    for (int col = 0; col < kNumColumns; ++col) {
        sf_count_t read = sf_readf_float(file, buf.data(), framesPerColumn);
        if (read <= 0) break;
        float mn = 0.f, mx = 0.f;
        for (sf_count_t i = 0; i < read; ++i) {
            float sum = 0.f;
            for (int c = 0; c < info.channels; ++c)
                sum += buf[static_cast<size_t>(i) * info.channels + c];
            sum /= static_cast<float>(info.channels);
            mn = std::min(mn, sum);
            mx = std::max(mx, sum);
        }
        peaksMin_[static_cast<size_t>(col)] = mn;
        peaksMax_[static_cast<size_t>(col)] = mx;
    }

    sf_close(file);
}

void WaveformView::clear() {
    peaksMin_.clear();
    peaksMax_.clear();
    numFrames_ = 0;
}

bool WaveformView::draw(float height, int64_t offsetFrames, int64_t loopStartFrame,
                        int64_t loopEndFrame) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + avail.x, p0.y + height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(15, 15, 18, 255));
    dl->AddRect(p0, p1, IM_COL32(60, 60, 65, 255));

    if (!hasData()) {
        const char* msg = "No sample loaded - click, or drag & drop a "
                          "WAV/AIFF/FLAC file here";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (avail.x - ts.x) * 0.5f, p0.y + (height - ts.y) * 0.5f),
                   IM_COL32(150, 150, 155, 255), msg);
    } else {
        float midY = p0.y + height * 0.5f;
        float halfH = height * 0.48f;
        int cols = static_cast<int>(peaksMin_.size());
        int pixelW = static_cast<int>(avail.x);
        for (int x = 0; x < pixelW; ++x) {
            int col = std::min(cols - 1, static_cast<int>(
                                             static_cast<float>(x) / avail.x * cols));
            float top = midY - peaksMax_[static_cast<size_t>(col)] * halfH;
            float bot = midY - peaksMin_[static_cast<size_t>(col)] * halfH;
            dl->AddLine(ImVec2(p0.x + x, top), ImVec2(p0.x + x, bot),
                       IM_COL32(120, 190, 255, 255));
        }

        auto frameToX = [&](int64_t frame) {
            float t = numFrames_ > 0 ? static_cast<float>(frame) / static_cast<float>(numFrames_)
                                     : 0.f;
            return p0.x + t * avail.x;
        };

        if (loopStartFrame >= 0) {
            float x = frameToX(loopStartFrame);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(80, 220, 100, 220), 1.5f);
        }
        if (loopEndFrame >= 0) {
            float x = frameToX(loopEndFrame);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(220, 100, 80, 220), 1.5f);
        }
        if (offsetFrames >= 0) {
            float x = frameToX(offsetFrames);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(240, 200, 40, 220), 1.5f);
        }
    }

    ImGui::InvisibleButton("##waveform_click", ImVec2(avail.x, height));
    return ImGui::IsItemClicked(0);
}
