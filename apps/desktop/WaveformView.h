// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Arrangement-style waveform display: read-only lanes, ruler, playhead,
// analysis markers, click-to-seek, zoom/scroll, and a time selection (drag
// across the lanes; Shift+click extends it, Esc clears it). With tracked
// beats the selection's ends snap to the beats, or to bars when zoomed out;
// Alt held while dragging selects freely. Draws from peak
// pyramids only, so cost is independent of file length. Track headers live in
// TrackPanel, which follows laneLayoutChanged. GPU rendering (QRhi) is planned
// once lane counts grow.

#include <daw/media/PeakPyramid.h>
#include <daw/project/ProjectModel.h>

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
    static constexpr int kRulerHeight = 40;
    static constexpr int kBarRowHeight = 20; // bars and beats above, time below

    explicit WaveformView(QWidget* parent = nullptr);

    /// The project rate and tempo; tracked beats draw the bars along them.
    void setTimeline(double projectRate, const daw::TempoMap& tempo);
    void setLanes(std::vector<WaveformLane> lanes);
    /// Updates only the dimming of each lane (cheap; used for mute/solo).
    void setLaneAudibility(const std::vector<bool>& audible);
    void setPlayhead(std::int64_t projectFrame);
    void setMarkers(std::vector<std::int64_t> projectFrames);
    /// true: one band per channel; false: channels combined into one band.
    void setSplitChannels(bool split);
    /// Whether selections snap to the tracked beats (no effect without them).
    void setSnapToBeats(bool snap);
    void zoomToFit();

    int laneHeight() const noexcept;
    void setScrollY(int y);
    void scrollByWheel(int angleDelta);

    /// The time selection in project frames, [start, end); empty when none.
    bool hasSelection() const noexcept { return selectionEnd_ > selectionStart_; }
    std::int64_t selectionStart() const noexcept { return selectionStart_; }
    std::int64_t selectionEnd() const noexcept { return selectionEnd_; }
    void clearSelection();

Q_SIGNALS:
    void seekRequested(qint64 projectFrame);
    /// The selection changed; start == end when it was cleared.
    void selectionChanged(qint64 start, qint64 end);
    /// A press on a lane (index into the lanes given to setLanes).
    void laneClicked(int lane);
    void laneLayoutChanged(int laneHeight, int scrollY);
    void verticalRangeChanged(int maximum, int pageStep);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    int waveformWidth() const noexcept { return std::max(1, width()); }
    int lanesViewportHeight() const noexcept { return std::max(1, height() - kRulerHeight); }
    int maxScrollY() const noexcept;
    double xForFrame(double frame) const noexcept;
    std::int64_t frameForX(double x) const noexcept;
    std::int64_t contentEnd() const noexcept;
    void drawRuler(class QPainter& painter);
    void drawBarLines(class QPainter& painter, int lanesBottom);
    /// Every how many bars a bar is labelled at this zoom (power of 2).
    int barLabelStep() const noexcept;
    double pixelsPerBeat() const noexcept;
    void drawClip(class QPainter& painter, const WaveformLane& lane, const WaveformClip& clip, const class QRect& laneRect);
    void seekToX(double x);
    void setSelection(std::int64_t a, std::int64_t b);
    /// The frame a selection end at `frame` snaps to (itself when snapping is
    /// off, Alt is held, or there are no tracked beats).
    std::int64_t snapped(std::int64_t frame, Qt::KeyboardModifiers modifiers) const;
    void drawSelection(class QPainter& painter, int lanesBottom);
    void updateVerticalLayout();

    enum class Drag { None, Scrub, Pending, Select };

    static constexpr int kMinLaneHeight = 72;
    static constexpr int kMaxLaneHeight = 140;
    static constexpr double kSnapPixels = 12.0; // the finest grid snapped to, on screen

    double projectRate_ = 48000.0;
    daw::TempoMap tempo_;
    std::vector<WaveformLane> lanes_;
    std::vector<std::int64_t> markers_;
    std::int64_t playhead_ = 0;
    std::int64_t selectionStart_ = 0;
    std::int64_t selectionEnd_ = 0;
    std::int64_t dragAnchor_ = 0; // the fixed end while selecting
    bool snapAnchor_ = false;     // the anchor is a click point, snapped with the other end
    bool snap_ = true;
    double pressX_ = 0.0;
    Drag drag_ = Drag::None;
    double framesPerPixel_ = 512.0;
    double viewStart_ = 0.0; // frame at the left edge
    int scrollY_ = 0;
    int lastLaneHeight_ = -1;
    int lastScrollY_ = -1;
    int lastMaxScroll_ = -1;
    bool splitChannels_ = true;
    bool fittedOnce_ = false;
};
