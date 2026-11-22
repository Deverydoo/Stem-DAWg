// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Audio device enumeration and lifecycle. JUCE-backed (WASAPI, DirectSound,
// optional ASIO) but the public interface is JUCE-free so Qt translation units
// never include JUCE headers.
//
// Requires a live ScopedAudioRuntime on the control thread. All methods must be
// called from that thread.

#include <daw/core/Result.h>

#include <memory>
#include <string>
#include <vector>

namespace daw {

class AudioEngine;

struct AudioDeviceSetup {
    std::string typeName;          // "Windows Audio", "DirectSound", "ASIO"; empty = current/default
    std::string outputDeviceName;  // empty = default device of the type
    double sampleRate = 0.0;       // 0 = device default
    int bufferSize = 0;            // 0 = device default

    friend bool operator==(const AudioDeviceSetup&, const AudioDeviceSetup&) = default;
};

struct AudioDeviceDescription {
    std::string typeName;
    std::string name;
    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;
    int defaultBufferSize = 0;
    int numOutputChannels = 0;
};

enum class DeviceState { Closed, Running, Error };

struct DeviceStatus {
    DeviceState state = DeviceState::Closed;
    std::string typeName;
    std::string deviceName;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int outputLatencySamples = 0;
    int driverXruns = -1;  // -1 if the driver does not report xruns
    std::string lastError;
};

class AudioDeviceService {
public:
    explicit AudioDeviceService(AudioEngine& engine);
    ~AudioDeviceService();

    AudioDeviceService(const AudioDeviceService&) = delete;
    AudioDeviceService& operator=(const AudioDeviceService&) = delete;

    std::vector<std::string> deviceTypes();
    std::vector<std::string> outputDevices(const std::string& typeName);
    Result<AudioDeviceDescription> describe(const std::string& typeName, const std::string& deviceName);

    /// Opens (or reconfigures) the output device and starts the engine callback.
    Status open(const AudioDeviceSetup& setup);
    void close();

    DeviceStatus status() const;
    AudioDeviceSetup currentSetup() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// RAII initialisation of the JUCE runtime (MessageManager) on the calling
/// thread. Create once, on the Qt main thread, before any AudioDeviceService.
/// JUCE posts its internal messages to a hidden window that Qt's native event
/// loop dispatches, so no separate JUCE loop is needed.
class ScopedAudioRuntime {
public:
    ScopedAudioRuntime();
    ~ScopedAudioRuntime();

    ScopedAudioRuntime(const ScopedAudioRuntime&) = delete;
    ScopedAudioRuntime& operator=(const ScopedAudioRuntime&) = delete;

    static std::string juceVersion();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace daw
