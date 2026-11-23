// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/dsp/LoudnessMeter.h>

#include <QLabel>

/// Compact live loudness readout for the transport bar: momentary,
/// short-term and integrated loudness, loudness range and true peak.
/// Clicking it restarts the measurement.
class LoudnessDisplay final : public QLabel {
    Q_OBJECT

public:
    explicit LoudnessDisplay(QWidget* parent = nullptr);

    void setReading(const daw::LoudnessReading& reading, bool playing);

Q_SIGNALS:
    void resetRequested();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    QString shown_;
};
