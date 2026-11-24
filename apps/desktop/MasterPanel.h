// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The master bus (milestone 5): tonal EQ, the true-peak limiter, the
// loudness target and the tonal-balance target.
//
// Match Loudness sets the master fader so the mastered mix reaches the
// loudness target through the limiter. Compare measures the mix's tonal
// balance against the chosen target (built from reference tracks with New
// Target) and suggests master EQ, which Apply Suggested EQ sets. The long
// jobs run in the background (ProjectController). Edits apply live and are
// undoable; the panel never owns the settings, setSettings() refreshes it.

#include <daw/project/ProjectModel.h>
#include <daw/session/MasteringTools.h>

#include <QWidget>

#include <array>
#include <optional>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class ReductionMeter;
class TonalChart;

class MasterPanel final : public QWidget {
    Q_OBJECT

public:
    explicit MasterPanel(QWidget* parent = nullptr);

    void setSettings(const daw::MasterSettings& settings, double masterGainDb);
    /// The saved tonal targets (the app's library).
    void setTargetLibrary(const std::vector<daw::TonalTargetSettings>& targets);
    /// Deepest limiter reduction since the last call (dB, <= 0).
    void setLimiterReduction(float db);
    /// While a match runs the button is disabled.
    void setMatching(bool running);
    void showMatch(const daw::LoudnessMatch& match, double targetLufs);
    void showMatchError(const QString& message);
    /// While a target is built or a comparison runs, those buttons are disabled.
    void setTonalBusy(bool running, const QString& message = {});
    void showComparison(const daw::TonalComparison& comparison, const daw::TonalTargetSettings& target);
    void showTonalError(const QString& message);

Q_SIGNALS:
    void settingsEdited(const daw::MasterSettings& settings);
    void matchRequested(double targetLufs);
    void newTargetRequested(const QString& folder);
    void compareRequested();

private:
    void edited();
    void presetChosen(int index);
    void syncPreset();
    void targetChosen(int index);
    void syncTarget();
    daw::MasterSettings readControls() const;

    QGroupBox* eq_ = nullptr;
    std::array<QDoubleSpinBox*, daw::MasterSettings::kEqBands> eqGains_{};
    QGroupBox* limiter_ = nullptr;
    QDoubleSpinBox* ceiling_ = nullptr;
    QDoubleSpinBox* release_ = nullptr;
    ReductionMeter* meter_ = nullptr;
    QComboBox* preset_ = nullptr;
    QDoubleSpinBox* target_ = nullptr;
    QPushButton* match_ = nullptr;
    QLabel* fader_ = nullptr;
    QLabel* result_ = nullptr;
    QComboBox* tonalTarget_ = nullptr;
    QPushButton* newTarget_ = nullptr;
    QPushButton* compare_ = nullptr;
    TonalChart* chart_ = nullptr;
    QLabel* tonalText_ = nullptr;
    QPushButton* applyEq_ = nullptr;

    daw::MasterSettings settings_;
    std::vector<daw::TonalTargetSettings> library_;
    std::optional<std::array<float, daw::MasterSettings::kEqBands>> suggestedEq_;
    bool updating_ = false;
};
