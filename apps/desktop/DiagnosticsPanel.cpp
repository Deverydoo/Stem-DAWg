// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "DiagnosticsPanel.h"

#include "HelpText.h"

#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QProgressBar* makeMeter(QWidget* parent)
{
    auto* bar = new QProgressBar(parent);
    bar->setRange(-600, 0); // tenths of dBFS
    bar->setValue(-600);
    bar->setTextVisible(true);
    bar->setFormat(QStringLiteral("-inf"));
    bar->setMaximumHeight(14);
    return bar;
}

void setMeter(QProgressBar* bar, double& display, float peak)
{
    const double db = peak > 0.0f ? std::max(-60.0, 20.0 * std::log10(static_cast<double>(peak))) : -60.0;
    display = std::max(db, display - 1.5); // ~45 dB/s fall-back at 30 Hz
    bar->setValue(static_cast<int>(display * 10.0));
    bar->setFormat(display <= -59.9 ? QStringLiteral("-inf") : QStringLiteral("%1 dB").arg(display, 0, 'f', 1));
}
} // namespace

DiagnosticsPanel::DiagnosticsPanel(QWidget* parent) : QWidget(parent)
{
    device_ = new QLabel(tr("closed"), this);
    format_ = new QLabel(this);
    load_ = new QLabel(this);
    problems_ = new QLabel(this);
    engine_ = new QLabel(this);
    ai_ = new QLabel(tr("initialising..."), this);
    ai_->setWordWrap(true);
    device_->setWordWrap(true);
    meterL_ = makeMeter(this);
    meterR_ = makeMeter(this);
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->setFont(QFont(QStringLiteral("Consolas"), 8));

    auto* form = new QFormLayout;
    const auto row = [&](const QString& label, QWidget* field, const QString& tip) {
        auto* name = new QLabel(label, this);
        name->setToolTip(tip);
        field->setToolTip(tip);
        form->addRow(name, field);
    };
    row(tr("Device"), device_,
        helpTip(tr("Audio device"), tr("The driver type and output device playing your audio. Change it in Audio > Device Settings.")));
    row(tr("Format"), format_,
        helpTip(tr("Device format"), tr("Sample rate, buffer size and output latency. Smaller buffers respond faster but need more CPU headroom.")));
    row(tr("DSP load"), load_,
        helpTip(tr("DSP load"), tr("How much of each audio deadline the engine uses. Above 100% the audio would drop out. The peak is the worst moment so far.")));
    row(tr("Dropouts"), problems_,
        helpTip(tr("Dropouts"), tr("Deadline misses: the engine was too slow for a block. Callback gaps: the driver paused (often other software or power saving). Both should stay at 0.")));
    row(tr("Engine"), engine_,
        helpTip(tr("Engine counters"), tr("Audio callbacks so far, mix updates delivered to the engine (one per edit), and commands that could not be queued (should be 0).")));
    row(tr("AI"), ai_,
        helpTip(tr("AI / analysis"), tr("The inference runtime and hardware provider, the start-up self-test result, and the background analyzers available.")));
    row(tr("Out L"), meterL_,
        helpTip(tr("Output level (left)"), tr("Peak level leaving the master, in dB below full scale. 0 dB is the limit: anything above it clips.")));
    row(tr("Out R"), meterR_,
        helpTip(tr("Output level (right)"), tr("Peak level leaving the master, in dB below full scale. 0 dB is the limit: anything above it clips.")));
    log_->setToolTip(helpTip(tr("Log"), tr("Live application log. The full structured log is in %LOCALAPPDATA%/StemDAWg/logs.")));

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(new QLabel(tr("Log"), this));
    layout->addWidget(log_, 1);
}

void DiagnosticsPanel::updateEngine(const daw::EngineDiagnostics& engine, const daw::DeviceStatus& device,
                                    const daw::MeterPeaks& peaks)
{
    if (device.state == daw::DeviceState::Closed && device.lastError.empty()) {
        device_->setText(tr("closed"));
        format_->clear();
    } else {
        device_->setText(QStringLiteral("%1 / %2%3")
                             .arg(QString::fromStdString(device.typeName), QString::fromStdString(device.deviceName),
                                  device.lastError.empty() ? QString() : QStringLiteral("\nerror: ") + QString::fromStdString(device.lastError)));
        const double latencyMs = device.sampleRate > 0.0 ? 1000.0 * (device.bufferSize + device.outputLatencySamples) / device.sampleRate : 0.0;
        format_->setText(tr("%1 Hz, %2 samples, ~%3 ms out")
                             .arg(device.sampleRate, 0, 'f', 0)
                             .arg(device.bufferSize)
                             .arg(latencyMs, 0, 'f', 1));
    }
    load_->setText(tr("%1 % (peak %2 %)").arg(engine.lastLoad * 100.0, 0, 'f', 1).arg(engine.peakLoad * 100.0, 0, 'f', 1));
    problems_->setText(tr("deadline misses %1, callback gaps %2, driver xruns %3")
                           .arg(engine.deadlineMisses)
                           .arg(engine.callbackGaps)
                           .arg(device.driverXruns < 0 ? tr("n/a") : QString::number(device.driverXruns)));
    engine_->setText(tr("callbacks %1, snapshots %2, dropped cmds %3")
                         .arg(engine.callbacks)
                         .arg(engine.snapshotSwaps)
                         .arg(engine.droppedCommands));
    setMeter(meterL_, displayL_, peaks.left);
    setMeter(meterR_, displayR_, peaks.right);
}

void DiagnosticsPanel::setAiStatus(const QString& text)
{
    ai_->setText(text);
}

void DiagnosticsPanel::appendLog(const QString& line)
{
    log_->appendPlainText(line);
}
