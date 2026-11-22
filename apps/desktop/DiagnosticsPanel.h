// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/engine/AudioDeviceService.h>
#include <daw/engine/AudioEngine.h>

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;

/// Engine health (load, deadline misses, xruns), device, meters, AI backend
/// status and the live structured log.
class DiagnosticsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit DiagnosticsPanel(QWidget* parent = nullptr);

    void updateEngine(const daw::EngineDiagnostics& engine, const daw::DeviceStatus& device, const daw::MeterPeaks& peaks);
    void setAiStatus(const QString& text);
    void appendLog(const QString& line);

private:
    QLabel* device_ = nullptr;
    QLabel* format_ = nullptr;
    QLabel* load_ = nullptr;
    QLabel* problems_ = nullptr;
    QLabel* engine_ = nullptr;
    QLabel* ai_ = nullptr;
    QProgressBar* meterL_ = nullptr;
    QProgressBar* meterR_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    double displayL_ = -60.0;
    double displayR_ = -60.0;
};
