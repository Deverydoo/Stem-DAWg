// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Left-hand track headers: name, mute, solo, volume fader and pan knob per
// track. Headers are positioned to line up with WaveformView's lanes and follow
// its vertical scroll (setLaneLayout). The panel only reports user edits; the
// ProjectController owns the values and pushes them back through setTracks.

#include <QColor>
#include <QSlider>
#include <QString>
#include <QWidget>

#include <vector>

class QLabel;
class QToolButton;

struct TrackHeaderState {
    QString id; // daw::TrackId string
    QString name;
    int number = 0; // 1-based display number
    QColor color;
    double gainDb = 0.0;
    double pan = 0.0;
    bool muted = false;
    bool soloed = false;
    bool audible = true; // false when muted or silenced by another track's solo
    enum class Cleanup { Off, On, Bypassed } cleanup = Cleanup::Off;
    QString cleanupSummary; // e.g. "HP 120 Hz 24 dB/oct, expander -44 dB"
};

/// Horizontal fader in dB with a fader taper (-inf .. +6 dB), a detent at
/// 0 dB, and double-click to reset.
class VolumeFader final : public QSlider {
    Q_OBJECT

public:
    explicit VolumeFader(QWidget* parent = nullptr);

    void setGainDb(double db); // does not emit
    double gainDb() const noexcept { return gainDb_; }

    static double positionToDb(int position);
    static int dbToPosition(double db);
    static QString formatDb(double db);

Q_SIGNALS:
    void gainDbChanged(double db);

protected:
    void mouseDoubleClickEvent(class QMouseEvent* event) override;

private:
    double gainDb_ = 0.0;
    bool updating_ = false;
};

/// Rotary pan control: drag up/right to pan right, Shift for fine, wheel to
/// step, double-click to centre.
class PanKnob final : public QWidget {
    Q_OBJECT

public:
    explicit PanKnob(QWidget* parent = nullptr);

    void setPan(double pan); // does not emit
    double pan() const noexcept { return pan_; }
    void setAccent(const QColor& color);
    QSize sizeHint() const override { return {26, 26}; }

    static QString formatPan(double pan);

Q_SIGNALS:
    void panChanged(double pan);

protected:
    void paintEvent(class QPaintEvent* event) override;
    void mousePressEvent(class QMouseEvent* event) override;
    void mouseMoveEvent(class QMouseEvent* event) override;
    void mouseDoubleClickEvent(class QMouseEvent* event) override;
    void wheelEvent(class QWheelEvent* event) override;

private:
    void setPanFromUser(double pan);

    double pan_ = 0.0;
    double dragStartPan_ = 0.0;
    QPointF dragOrigin_;
    QColor accent_{0xd9, 0x8a, 0x4a};
};

class TrackHeader final : public QWidget {
    Q_OBJECT

public:
    explicit TrackHeader(QWidget* parent = nullptr);

    void setState(const TrackHeaderState& state);
    const QString& trackId() const noexcept { return state_.id; }

Q_SIGNALS:
    void gainChanged(double gainDb);
    void panChanged(double pan);
    void muteToggled(bool muted);
    /// `additive`: Ctrl was held, keep the other soloed tracks.
    void soloToggled(bool soloed, bool additive);
    void cleanupRequested();
    void renameRequested();
    void deleteRequested();

protected:
    void paintEvent(class QPaintEvent* event) override;
    void contextMenuEvent(class QContextMenuEvent* event) override;
    void mouseDoubleClickEvent(class QMouseEvent* event) override;
    void resizeEvent(class QResizeEvent* event) override;

private:
    void updateNameText();

    TrackHeaderState state_;
    QLabel* number_ = nullptr;
    QLabel* name_ = nullptr;
    QToolButton* mute_ = nullptr;
    QToolButton* solo_ = nullptr;
    QToolButton* cleanup_ = nullptr;
    VolumeFader* fader_ = nullptr;
    QLabel* gainLabel_ = nullptr;
    PanKnob* pan_ = nullptr;
    QLabel* panLabel_ = nullptr;
};

class TrackPanel final : public QWidget {
    Q_OBJECT

public:
    explicit TrackPanel(int rulerHeight, QWidget* parent = nullptr);

    /// Creates, updates or removes headers. Existing headers are reused (by
    /// track id) so a fader being dragged is never destroyed under the mouse.
    void setTracks(const std::vector<TrackHeaderState>& tracks);
    void setTopText(const QString& text);
    void setLaneLayout(int laneHeight, int scrollY);

Q_SIGNALS:
    void gainChanged(const QString& trackId, double gainDb);
    void panChanged(const QString& trackId, double pan);
    void muteToggled(const QString& trackId, bool muted);
    void soloToggled(const QString& trackId, bool soloed, bool additive);
    void cleanupRequested(const QString& trackId);
    void renameRequested(const QString& trackId);
    void deleteRequested(const QString& trackId);
    void scrollRequested(int angleDelta);

protected:
    void resizeEvent(class QResizeEvent* event) override;
    void wheelEvent(class QWheelEvent* event) override;

private:
    void layoutHeaders();

    QLabel* top_ = nullptr;
    QWidget* lanes_ = nullptr;
    std::vector<TrackHeader*> headers_; // display order
    int laneHeight_ = 80;
    int scrollY_ = 0;
};
