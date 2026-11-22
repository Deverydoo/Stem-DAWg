// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Arrangement-style waveform display (Milestone 1: read-only lanes, ruler,
// playhead, analysis markers, click-to-seek, zoom/scroll). Draws from peak
// pyramids only, so cost is independent of file length. Track headers live in
// TrackPanel, which follows laneLayoutChanged. Clip editing arrives in
// Milestone 2; GPU rendering (QRhi) is planned once lane counts grow.

#include <daw/media/PeakPyramid.h>

#include <QColor>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

struct WaveformClip {
    std::shared_ptr<const daw::PeakPyramid> peaks; // null while loading
    std::int64_t timelineStart = 0; // project frames
    std::int64_t sourceStart = 0;   // project frames
    std::int64_t length = 0;        // project frames
    double mediaRate = 0.0;
    QString name;
};

struct WaveformLane {
    QString name;
    QColor color;
    bool audible = true; // drawn dimmed when false (muted or not soloed)
    std::vector<WaveformClip> clips;
};

class WaveformView final : public QWidget {
    Q_OBJECT

public:
    static constexpr int kRulerHeight = 26;

    explicit WaveformView(QWidget* parent = nullptr);

    void setTimeline(double projectRate, double bpm, int beatsPerBar);
    void setLanes(std::vector<WaveformLane> lanes);
    /// Updates only the dimming of each lane (cheap; used for mute/solo).
    void setLaneAudibility(const std::vector<bool>& audible);
    void setPlayhead(std::int64_t projectFrame);
    void setMarkers(std::vector<std::int64_t> projectFrames);
    /// true: one band per channel; false: channels combined into one band.
    void setSplitChannels(bool split);
    void zoomToFit();

    int laneHeight() const noexcept;
    void setScrollY(int y);
    void scrollByWheel(int angleDelta);

Q_SIGNALS:
    void seekRequested(qint64 projectFrame);
    void laneLayoutChanged(int laneHeight, int scrollY);
    void verticalRangeChanged(int maximum, int pageStep);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    int waveformWidth() const noexcept { return std::max(1, width()); }
    int lanesViewportHeight() const noexcept { return std::max(1, height() - kRulerHeight); }
    int maxScrollY() const noexcept;
    double xForFrame(double frame) const noexcept;
    std::int64_t frameForX(double x) const noexcept;
    std::int64_t contentEnd() const noexcept;
    void drawRuler(class QPainter& painter);
    void drawClip(class QPainter& painter, const WaveformLane& lane, const WaveformClip& clip, const class QRect& laneRect);
    void seekToX(double x);
    void updateVerticalLayout();

    static constexpr int kMinLaneHeight = 72;
    static constexpr int kMaxLaneHeight = 140;

    double projectRate_ = 48000.0;
    double bpm_ = 120.0;
    int beatsPerBar_ = 4;
    std::vector<WaveformLane> lanes_;
    std::vector<std::int64_t> markers_;
    std::int64_t playhead_ = 0;
    double framesPerPixel_ = 512.0;
    double viewStart_ = 0.0; // frame at the left edge
    int scrollY_ = 0;
    int lastLaneHeight_ = -1;
    int lastScrollY_ = -1;
    int lastMaxScroll_ = -1;
    bool splitChannels_ = true;
    bool fittedOnce_ = false;
};
