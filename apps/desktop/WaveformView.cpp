// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "WaveformView.h"

#include "HelpText.h"

#include <QMouseEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QPixmap>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
const QColor kBackground(0x17, 0x18, 0x1b);
const QColor kLaneA(0x1c, 0x1e, 0x22);
const QColor kLaneB(0x1f, 0x21, 0x25);
const QColor kRuler(0x26, 0x28, 0x2d);
const QColor kText(0xc8, 0xc8, 0xc8);
const QColor kPlayhead(0xff, 0x5a, 0x1f);
const QColor kMarker(0xf2, 0xd0, 0x3b, 150);
const QColor kGrid(0x33, 0x35, 0x3b);
const QColor kDefaultTrack(0xd9, 0x8a, 0x4a);

QString formatTime(double seconds, double step)
{
    const int minutes = static_cast<int>(seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    if (step < 1.0)
        return QStringLiteral("%1:%2").arg(minutes).arg(rest, 4, 'f', 1, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(static_cast<int>(rest), 2, 10, QLatin1Char('0'));
}
} // namespace

WaveformView::WaveformView(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(200);
    setMouseTracking(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
}

bool WaveformView::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        if (help->pos().y() < kRulerHeight) {
            QToolTip::showText(help->globalPos(),
                               helpTip(tr("Timeline"), tr("Time in minutes:seconds. Click or drag anywhere to move the playhead.\n"
                                                          "Ctrl+wheel zooms, Shift+wheel scrolls sideways, wheel scrolls tracks.")),
                               this);
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return QWidget::event(event);
}

void WaveformView::setTimeline(double projectRate, double bpm, int beatsPerBar)
{
    projectRate_ = projectRate > 0.0 ? projectRate : 48000.0;
    bpm_ = bpm;
    beatsPerBar_ = beatsPerBar;
    update();
}

void WaveformView::setLanes(std::vector<WaveformLane> lanes)
{
    lanes_ = std::move(lanes);
    if (!fittedOnce_ && contentEnd() > 0) {
        zoomToFit();
        fittedOnce_ = true;
    }
    if (lanes_.empty())
        fittedOnce_ = false;
    updateVerticalLayout();
    update();
}

void WaveformView::setLaneAudibility(const std::vector<bool>& audible)
{
    bool changed = false;
    for (std::size_t i = 0; i < std::min(audible.size(), lanes_.size()); ++i)
        if (lanes_[i].audible != audible[i]) {
            lanes_[i].audible = audible[i];
            changed = true;
        }
    if (changed)
        update();
}

void WaveformView::setSplitChannels(bool split)
{
    if (split == splitChannels_)
        return;
    splitChannels_ = split;
    update();
}

void WaveformView::setPlayhead(std::int64_t projectFrame)
{
    if (projectFrame == playhead_)
        return;
    playhead_ = projectFrame;
    // Page-follow while the playhead runs off either edge.
    const double x = xForFrame(static_cast<double>(projectFrame));
    if (x < 0.0 || x > waveformWidth())
        viewStart_ = std::max(0.0, static_cast<double>(projectFrame) - 0.1 * waveformWidth() * framesPerPixel_);
    update();
}

void WaveformView::setMarkers(std::vector<std::int64_t> projectFrames)
{
    markers_ = std::move(projectFrames);
    update();
}

void WaveformView::zoomToFit()
{
    const std::int64_t end = contentEnd();
    framesPerPixel_ = end > 0 ? std::max(1.0, static_cast<double>(end) * 1.02 / waveformWidth()) : 512.0;
    viewStart_ = 0.0;
    update();
}

int WaveformView::laneHeight() const noexcept
{
    const int count = std::max<int>(1, static_cast<int>(lanes_.size()));
    return std::clamp(lanesViewportHeight() / count, kMinLaneHeight, kMaxLaneHeight);
}

int WaveformView::maxScrollY() const noexcept
{
    return std::max(0, static_cast<int>(lanes_.size()) * laneHeight() - lanesViewportHeight());
}

void WaveformView::setScrollY(int y)
{
    y = std::clamp(y, 0, maxScrollY());
    if (y == scrollY_)
        return;
    scrollY_ = y;
    updateVerticalLayout();
    update();
}

void WaveformView::scrollByWheel(int angleDelta)
{
    setScrollY(scrollY_ - angleDelta * laneHeight() / 240); // half a lane per notch
}

void WaveformView::updateVerticalLayout()
{
    scrollY_ = std::clamp(scrollY_, 0, maxScrollY());
    const int lane = laneHeight();
    const int maxScroll = maxScrollY();
    if (maxScroll != lastMaxScroll_) {
        lastMaxScroll_ = maxScroll;
        Q_EMIT verticalRangeChanged(maxScroll, lanesViewportHeight());
    }
    if (lane != lastLaneHeight_ || scrollY_ != lastScrollY_) {
        lastLaneHeight_ = lane;
        lastScrollY_ = scrollY_;
        Q_EMIT laneLayoutChanged(lane, scrollY_);
    }
}

void WaveformView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateVerticalLayout();
}

std::int64_t WaveformView::contentEnd() const noexcept
{
    std::int64_t end = 0;
    for (const auto& lane : lanes_)
        for (const auto& clip : lane.clips)
            end = std::max(end, clip.timelineStart + clip.length);
    return end;
}

double WaveformView::xForFrame(double frame) const noexcept
{
    return (frame - viewStart_) / framesPerPixel_;
}

std::int64_t WaveformView::frameForX(double x) const noexcept
{
    return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(viewStart_ + x * framesPerPixel_)));
}

void WaveformView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    const int lane = laneHeight();
    painter.save();
    painter.setClipRect(QRect(0, kRulerHeight, width(), lanesViewportHeight()));
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        const QRect laneRect(0, kRulerHeight + i * lane - scrollY_, waveformWidth(), lane);
        if (laneRect.bottom() < kRulerHeight || laneRect.top() > height())
            continue;
        painter.fillRect(laneRect, i % 2 == 0 ? kLaneA : kLaneB);
        const WaveformLane& laneData = lanes_[static_cast<std::size_t>(i)];
        for (const auto& clip : laneData.clips)
            drawClip(painter, laneData, clip, laneRect);
        painter.setPen(kGrid);
        painter.drawLine(laneRect.bottomLeft(), laneRect.bottomRight());
    }

    // Analysis markers (transient candidates).
    const int lanesBottom = std::min(height(), kRulerHeight + static_cast<int>(lanes_.size()) * lane - scrollY_);
    painter.setPen(QPen(kMarker, 1));
    for (std::int64_t marker : markers_) {
        const double x = xForFrame(static_cast<double>(marker));
        if (x >= 0.0 && x <= width())
            painter.drawLine(QPointF(x, kRulerHeight), QPointF(x, lanesBottom));
    }
    painter.restore();

    if (lanes_.empty()) {
        // Empty arrangement: show the logo and how to start.
        static const QPixmap logo(QStringLiteral(":/branding/stem-dawg-logo-about.png"));
        const QRect area = rect().adjusted(0, kRulerHeight, 0, 0);
        int textTop = area.center().y();
        if (!logo.isNull()) {
            const QPixmap scaled = logo.scaled(std::min(420, area.width() - 40), std::min(280, area.height() - 80),
                                               Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const QPoint topLeft(area.center().x() - scaled.width() / 2, area.center().y() - scaled.height() / 2 - 20);
            painter.setOpacity(0.85);
            painter.drawPixmap(topLeft, scaled);
            painter.setOpacity(1.0);
            textTop = topLeft.y() + scaled.height() + 12;
        }
        painter.setPen(kText);
        painter.drawText(QRect(area.left(), textTop, area.width(), 30), Qt::AlignHCenter | Qt::AlignTop,
                         tr("Import WAV / FLAC / MP3 stems (Ctrl+I) or drop files here"));
    }

    drawRuler(painter);

    const double playheadX = xForFrame(static_cast<double>(playhead_));
    if (playheadX >= 0.0 && playheadX <= width()) {
        painter.setPen(QPen(kPlayhead, 2));
        painter.drawLine(QPointF(playheadX, 0), QPointF(playheadX, height()));
    }
}

void WaveformView::drawRuler(QPainter& painter)
{
    const QRect ruler(0, 0, width(), kRulerHeight);
    painter.fillRect(ruler, kRuler);
    painter.setPen(kGrid);
    painter.drawLine(0, kRulerHeight - 1, width(), kRulerHeight - 1);

    static constexpr double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
    const double secondsPerPixel = framesPerPixel_ / projectRate_;
    double step = steps[std::size(steps) - 1];
    for (double s : steps)
        if (s / secondsPerPixel >= 90.0) {
            step = s;
            break;
        }

    const double firstSecond = std::floor((viewStart_ / projectRate_) / step) * step;
    painter.setPen(kText);
    for (double t = firstSecond;; t += step) {
        const double x = xForFrame(t * projectRate_);
        if (x > width())
            break;
        if (x < 0.0)
            continue;
        painter.drawLine(QPointF(x, kRulerHeight - 8), QPointF(x, kRulerHeight));
        painter.drawText(QPointF(x + 3, kRulerHeight - 10), formatTime(t, step));
    }
}

void WaveformView::drawClip(QPainter& painter, const WaveformLane& lane, const WaveformClip& clip, const QRect& laneRect)
{
    const double x0 = std::max<double>(xForFrame(static_cast<double>(clip.timelineStart)), laneRect.left());
    const double x1 = std::min<double>(xForFrame(static_cast<double>(clip.timelineStart + clip.length)), laneRect.right());
    if (x1 <= x0)
        return;

    const QColor base = lane.color.isValid() ? lane.color : kDefaultTrack;
    QColor fill = base.darker(lane.audible ? 230 : 420);
    QColor wave = lane.audible ? base.lighter(125) : base.darker(200);
    QColor label = lane.audible ? QColor(0xf0, 0xf0, 0xf0) : QColor(0x80, 0x80, 0x80);

    const QRectF clipRect(x0, laneRect.top() + 2, x1 - x0, laneRect.height() - 5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(clipRect, 4, 4);
    painter.setPen(label);
    painter.drawText(clipRect.adjusted(6, 2, -4, 0), Qt::AlignLeft | Qt::AlignTop, clip.name);

    if (!clip.peaks || clip.peaks->empty() || clip.mediaRate <= 0.0) {
        painter.drawText(clipRect, Qt::AlignCenter, tr("loading..."));
        return;
    }

    const double mediaPerProject = clip.mediaRate / projectRate_;
    const daw::PeakLevel& level = clip.peaks->levelForResolution(framesPerPixel_ * mediaPerProject);
    const int channels = clip.peaks->numChannels();
    const int bands = splitChannels_ ? channels : 1;
    const double top = clipRect.top() + 16.0;
    const double bandHeight = (clipRect.bottom() - 2.0 - top) / bands;

    std::vector<QLineF> lines;
    lines.reserve(static_cast<std::size_t>((x1 - x0) * bands) + 1);
    for (int px = static_cast<int>(x0); px < static_cast<int>(x1); ++px) {
        const double f0 = viewStart_ + px * framesPerPixel_ - clip.timelineStart + clip.sourceStart;
        const double f1 = f0 + framesPerPixel_;
        const auto m0 = static_cast<std::int64_t>(f0 * mediaPerProject);
        const auto m1 = std::max(m0 + 1, static_cast<std::int64_t>(f1 * mediaPerProject));
        float combinedMin = 0.0f, combinedMax = 0.0f;
        for (int c = 0; c < channels; ++c) {
            const daw::PeakBucket peak = clip.peaks->query(c, m0, m1, level);
            if (!splitChannels_) {
                combinedMin = std::min(combinedMin, peak.min);
                combinedMax = std::max(combinedMax, peak.max);
                continue;
            }
            const double centre = top + bandHeight * (c + 0.5);
            const double half = bandHeight * 0.48;
            lines.emplace_back(px + 0.5, centre - std::clamp(peak.max, -1.0f, 1.0f) * half, px + 0.5,
                               centre - std::clamp(peak.min, -1.0f, 1.0f) * half + 0.5);
        }
        if (!splitChannels_) {
            const double centre = top + bandHeight * 0.5;
            const double half = bandHeight * 0.48;
            lines.emplace_back(px + 0.5, centre - std::clamp(combinedMax, -1.0f, 1.0f) * half, px + 0.5,
                               centre - std::clamp(combinedMin, -1.0f, 1.0f) * half + 0.5);
        }
    }
    painter.setPen(QPen(wave, 1));
    painter.drawLines(lines.data(), static_cast<int>(lines.size()));
}

void WaveformView::seekToX(double x)
{
    const std::int64_t frame = frameForX(x);
    playhead_ = frame;
    update();
    Q_EMIT seekRequested(frame);
}

void WaveformView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        seekToX(event->position().x());
}

void WaveformView::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton)
        seekToX(event->position().x());
}

void WaveformView::wheelEvent(QWheelEvent* event)
{
    const double notches = event->angleDelta().y() / 120.0;
    if (event->modifiers() & Qt::ControlModifier) {
        // Zoom around the cursor.
        const double anchorX = event->position().x();
        const double anchorFrame = viewStart_ + anchorX * framesPerPixel_;
        framesPerPixel_ = std::clamp(framesPerPixel_ * std::pow(1.25, -notches), 1.0, 1.0e6);
        viewStart_ = std::max(0.0, anchorFrame - anchorX * framesPerPixel_);
    } else if (event->angleDelta().x() != 0 || (event->modifiers() & Qt::ShiftModifier) || maxScrollY() == 0) {
        // Horizontal: trackpad sideways, Shift+wheel, or wheel when all lanes fit.
        const double delta = event->angleDelta().x() != 0 ? event->angleDelta().x() / 120.0 : notches;
        viewStart_ = std::max(0.0, viewStart_ - delta * 0.1 * waveformWidth() * framesPerPixel_);
    } else {
        scrollByWheel(event->angleDelta().y());
    }
    update();
    event->accept();
}
