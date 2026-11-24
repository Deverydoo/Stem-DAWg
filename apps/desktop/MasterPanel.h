// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The master bus (milestone 5): the true-peak limiter after the summed
// tracks, and the loudness target. Match Loudness sets the master fader so
// the mastered mix reaches the target through the limiter (it runs in the
// background; see daw::matchLoudness). Edits apply live and are undoable;
// the panel never owns the settings, setSettings() refreshes it.

#include <daw/project/ProjectModel.h>
#include <daw/session/MasteringTools.h>

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class ReductionMeter;

class MasterPanel final : public QWidget {
    Q_OBJECT

public:
    explicit MasterPanel(QWidget* parent = nullptr);

    void setSettings(const daw::MasterSettings& settings, double masterGainDb);
    /// Deepest limiter reduction since the last call (dB, <= 0).
    void setLimiterReduction(float db);
    /// While a match runs the button is disabled.
    void setMatching(bool running);
    void showMatch(const daw::LoudnessMatch& match, double targetLufs);
    void showMatchError(const QString& message);

Q_SIGNALS:
    void settingsEdited(const daw::MasterSettings& settings);
    void matchRequested(double targetLufs);

private:
    void edited();
    void presetChosen(int index);
    void syncPreset();
    daw::MasterSettings readControls() const;

    QGroupBox* limiter_ = nullptr;
    QDoubleSpinBox* ceiling_ = nullptr;
    QDoubleSpinBox* release_ = nullptr;
    ReductionMeter* meter_ = nullptr;
    QComboBox* preset_ = nullptr;
    QDoubleSpinBox* target_ = nullptr;
    QPushButton* match_ = nullptr;
    QLabel* fader_ = nullptr;
    QLabel* result_ = nullptr;
    daw::MasterSettings settings_;
    bool updating_ = false;
};
