// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Editor for one track's clean-up chain (DC removal, high-pass, hum removal,
// noise reduction, expander).
// Edits apply live: every change is reported through cleanupEdited and the
// ProjectController pushes it to the engine. The panel never owns the
// settings; setTracks() refreshes it from the project.

#include <daw/project/ProjectModel.h>

#include <QColor>
#include <QWidget>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QAction;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;
class ReductionMeter;

struct CleanupTrackInfo {
    QString id; // daw::TrackId string
    QString name;
    QColor color;
    daw::TrackCleanup cleanup;
    std::optional<double> noiseFloorDbfs; // from the stem report, if any
    bool floorAfterExpander = false;      // the floor was measured with the expander running
    std::optional<double> voiceDenoiseDb; // the track plays a voice-denoised copy at this reduction
    bool voiceDenoiseRunning = false;     // a voice denoise is rendering (any track)
};

class CleanupPanel final : public QWidget {
    Q_OBJECT

public:
    explicit CleanupPanel(QWidget* parent = nullptr);

    void setTracks(const std::vector<CleanupTrackInfo>& tracks);
    void selectTrack(const QString& id);
    QString currentTrackId() const;
    /// Expander gain reduction of the shown track (dB, <= 0); the meter decays on its own.
    void setGainReduction(float db);
    /// Shows a button for the window's "Listen to Removed" action.
    void setListenAction(QAction* action);
    /// With a time selection, Learn Noise and Detect measure only it (their labels say so).
    void setSelectionActive(bool active);

Q_SIGNALS:
    void cleanupEdited(const QString& trackId, const daw::TrackCleanup& cleanup);
    void learnNoiseRequested(const QString& trackId);
    void detectHumRequested(const QString& trackId);
    void voiceDenoiseRequested(const QString& trackId, double reductionDb);
    void voiceRevertRequested(const QString& trackId);

private:
    const CleanupTrackInfo* current() const;
    void showCurrent();
    void edited();
    daw::TrackCleanup readControls() const;

    std::vector<CleanupTrackInfo> tracks_;
    QComboBox* track_ = nullptr;
    QCheckBox* bypass_ = nullptr;
    QPushButton* reset_ = nullptr;
    QCheckBox* dc_ = nullptr;
    QGroupBox* highPass_ = nullptr;
    QSlider* hpSlider_ = nullptr;
    QDoubleSpinBox* hpFrequency_ = nullptr;
    QComboBox* hpSlope_ = nullptr;
    QGroupBox* voice_ = nullptr;
    QLabel* voiceState_ = nullptr;
    QComboBox* voiceReduction_ = nullptr;
    QPushButton* voiceApply_ = nullptr;
    QPushButton* voiceRevert_ = nullptr;
    QGroupBox* hum_ = nullptr;
    QComboBox* humMains_ = nullptr;
    QSpinBox* humHarmonics_ = nullptr;
    QPushButton* detectHum_ = nullptr;
    QGroupBox* denoise_ = nullptr;
    QPushButton* learn_ = nullptr;
    QLabel* profile_ = nullptr;
    QSlider* reductionSlider_ = nullptr;
    QDoubleSpinBox* reduction_ = nullptr;
    QDoubleSpinBox* sensitivity_ = nullptr;
    QToolButton* listen_ = nullptr;
    QGroupBox* expander_ = nullptr;
    QSlider* thresholdSlider_ = nullptr;
    QDoubleSpinBox* threshold_ = nullptr;
    QDoubleSpinBox* ratio_ = nullptr;
    QDoubleSpinBox* range_ = nullptr;
    QDoubleSpinBox* release_ = nullptr;
    ReductionMeter* meter_ = nullptr;
    QLabel* floor_ = nullptr;
    QPushButton* fromFloor_ = nullptr;
    QLabel* empty_ = nullptr;
    QWidget* editor_ = nullptr;
    class QScrollArea* scroll_ = nullptr;
    bool updating_ = false;
};
