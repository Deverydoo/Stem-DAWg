// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "EngineBridge.h"

#include <daw/core/Logger.h>

#include <QDateTime>

EngineBridge::EngineBridge(QObject* parent)
    : QObject(parent),
      runtime_(std::make_unique<daw::ScopedAudioRuntime>()),
      engine_(std::make_unique<daw::AudioEngine>()),
      devices_(std::make_unique<daw::AudioDeviceService>(*engine_))
{
    daw::logging::info("audio", "runtime initialised", {{"juce", daw::ScopedAudioRuntime::juceVersion()}});
    timer_.setInterval(33);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &EngineBridge::poll);
    timer_.start();
}

EngineBridge::~EngineBridge()
{
    timer_.stop();
    devices_.reset(); // closes the device; no more audio callbacks after this
    engine_->collectGarbage();
}

daw::Status EngineBridge::openDevice(const daw::AudioDeviceSetup& setup)
{
    auto status = devices_->open(setup);
    checkRate();
    return status;
}

void EngineBridge::closeDevice()
{
    devices_->close();
    checkRate();
}

double EngineBridge::sampleRate() const
{
    const auto status = devices_->status();
    return status.state == daw::DeviceState::Running ? status.sampleRate : 0.0;
}

void EngineBridge::play()
{
    engine_->play();
}

void EngineBridge::stop()
{
    engine_->stop();
}

void EngineBridge::togglePlay()
{
    if (engine_->transportState().playing)
        engine_->stop();
    else
        engine_->play();
}

void EngineBridge::seek(std::int64_t engineFrame)
{
    engine_->seek(engineFrame);
}

void EngineBridge::checkRate()
{
    const double rate = sampleRate();
    if (rate != lastRate_) {
        lastRate_ = rate;
        Q_EMIT sampleRateChanged(rate);
    }
}

void EngineBridge::poll()
{
    engine_->collectGarbage();

    // Drain audio-thread events. Timing problems are aggregated so a struggling
    // system is not flooded with log lines.
    daw::RtEvent event;
    while (engine_->popEvent(event)) {
        switch (event.code) {
        case daw::RtEventCode::DeadlineMiss: ++pendingDeadlineMisses_; break;
        case daw::RtEventCode::CallbackGap: ++pendingGaps_; break;
        case daw::RtEventCode::SnapshotAdopted: break;
        default:
            daw::logging::info("engine", daw::toString(event.code), {{"a", event.a}, {"b", event.b}});
            break;
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((pendingDeadlineMisses_ > 0 || pendingGaps_ > 0) && now - lastRtSummaryMs_ > 1000) {
        daw::logging::warn("engine", "audio timing problems",
                           {{"deadlineMisses", pendingDeadlineMisses_}, {"callbackGaps", pendingGaps_}});
        pendingDeadlineMisses_ = 0;
        pendingGaps_ = 0;
        lastRtSummaryMs_ = now;
    }

    checkRate();
    Q_EMIT tick();
}
