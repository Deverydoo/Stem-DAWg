// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Owns the audio runtime, engine and device service on the Qt main thread and
// turns lock-free engine state into Qt signals at a fixed UI rate (30 Hz).
// The audio thread never touches Qt.

#include <daw/engine/AudioDeviceService.h>
#include <daw/engine/AudioEngine.h>

#include <QObject>
#include <QTimer>

#include <memory>

class EngineBridge final : public QObject {
    Q_OBJECT

public:
    explicit EngineBridge(QObject* parent = nullptr);
    ~EngineBridge() override;

    daw::AudioEngine& engine() noexcept { return *engine_; }
    daw::AudioDeviceService& devices() noexcept { return *devices_; }

    daw::Status openDevice(const daw::AudioDeviceSetup& setup);
    void closeDevice();
    /// Engine rate while a device is running, otherwise 0.
    double sampleRate() const;

    void play();
    void stop();
    void togglePlay();
    void seek(std::int64_t engineFrame);
    daw::TransportState transport() const { return engine_->transportState(); }

Q_SIGNALS:
    void tick();
    void sampleRateChanged(double rate);

private:
    void poll();
    void checkRate();

    std::unique_ptr<daw::ScopedAudioRuntime> runtime_;
    std::unique_ptr<daw::AudioEngine> engine_;
    std::unique_ptr<daw::AudioDeviceService> devices_; // destroyed first: stops callbacks
    QTimer timer_;
    double lastRate_ = 0.0;
    std::uint64_t pendingDeadlineMisses_ = 0;
    std::uint64_t pendingGaps_ = 0;
    qint64 lastRtSummaryMs_ = 0;
};
