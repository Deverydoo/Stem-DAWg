// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/engine/AudioDeviceService.h>

#include <daw/core/Logger.h>
#include <daw/engine/AudioEngine.h>

#include <juce_audio_devices/juce_audio_devices.h>

#include <mutex>

namespace daw {

namespace {

juce::String toJuce(const std::string& utf8)
{
    return juce::String::fromUTF8(utf8.c_str());
}

std::string fromJuce(const juce::String& text)
{
    return text.toStdString();
}

} // namespace

struct AudioDeviceService::Impl final : juce::AudioIODeviceCallback {
    explicit Impl(AudioEngine& e) : engine(e) { manager.addAudioCallback(this); }

    ~Impl() override
    {
        manager.removeAudioCallback(this);
        manager.closeAudioDevice();
    }

    // juce::AudioIODeviceCallback --------------------------------------------
    void audioDeviceIOCallbackWithContext(const float* const* /*inputs*/, int /*numInputs*/, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& /*context*/) override
    {
        engine.process(outputs, numOutputs, numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        EngineConfig config;
        config.sampleRate = device->getCurrentSampleRate();
        config.maxBlockSize = device->getCurrentBufferSizeSamples();
        config.numOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
        engine.prepare(config);
        engine.beginProcessing();
    }

    void audioDeviceStopped() override { engine.endProcessing(); }

    void audioDeviceError(const juce::String& message) override
    {
        std::lock_guard lock(errorMutex);
        lastError = fromJuce(message);
    }

    juce::AudioIODeviceType* findType(const std::string& typeName)
    {
        for (auto* type : manager.getAvailableDeviceTypes())
            if (fromJuce(type->getTypeName()) == typeName)
                return type;
        return nullptr;
    }

    AudioEngine& engine;
    juce::AudioDeviceManager manager;
    bool initialised = false;
    mutable std::mutex errorMutex;
    std::string lastError;
};

AudioDeviceService::AudioDeviceService(AudioEngine& engine) : impl_(std::make_unique<Impl>(engine)) {}
AudioDeviceService::~AudioDeviceService() = default;

std::vector<std::string> AudioDeviceService::deviceTypes()
{
    std::vector<std::string> names;
    for (auto* type : impl_->manager.getAvailableDeviceTypes())
        names.push_back(fromJuce(type->getTypeName()));
    return names;
}

std::vector<std::string> AudioDeviceService::outputDevices(const std::string& typeName)
{
    std::vector<std::string> names;
    if (auto* type = impl_->findType(typeName)) {
        type->scanForDevices();
        for (const auto& name : type->getDeviceNames(false))
            names.push_back(fromJuce(name));
    }
    return names;
}

Result<AudioDeviceDescription> AudioDeviceService::describe(const std::string& typeName, const std::string& deviceName)
{
    auto* type = impl_->findType(typeName);
    if (type == nullptr)
        return makeError(ErrorCode::NotFound, "unknown device type: " + typeName);

    AudioDeviceDescription description;
    description.typeName = typeName;
    description.name = deviceName;

    auto fill = [&](juce::AudioIODevice& device) {
        for (double rate : device.getAvailableSampleRates())
            description.sampleRates.push_back(rate);
        for (int size : device.getAvailableBufferSizes())
            description.bufferSizes.push_back(size);
        description.defaultBufferSize = device.getDefaultBufferSize();
        description.numOutputChannels = device.getOutputChannelNames().size();
    };

    auto* current = impl_->manager.getCurrentAudioDevice();
    if (current != nullptr && fromJuce(current->getTypeName()) == typeName && fromJuce(current->getName()) == deviceName) {
        fill(*current);
        return description;
    }

    std::unique_ptr<juce::AudioIODevice> device(type->createDevice(toJuce(deviceName), {}));
    if (!device)
        return makeError(ErrorCode::DeviceError, "cannot create device: " + deviceName);
    fill(*device);
    return description;
}

Status AudioDeviceService::open(const AudioDeviceSetup& setup)
{
    auto& manager = impl_->manager;
    {
        std::lock_guard lock(impl_->errorMutex);
        impl_->lastError.clear();
    }

    if (!impl_->initialised) {
        const juce::String error = manager.initialise(0, 2, nullptr, false);
        impl_->initialised = true;
        if (error.isNotEmpty())
            logging::warn("audio", "default device initialisation failed", {{"error", fromJuce(error)}});
    }

    if (!setup.typeName.empty() && fromJuce(manager.getCurrentAudioDeviceType()) != setup.typeName) {
        if (impl_->findType(setup.typeName) == nullptr)
            return makeError(ErrorCode::NotFound, "unknown device type: " + setup.typeName);
        manager.setCurrentAudioDeviceType(toJuce(setup.typeName), true);
    }

    juce::AudioDeviceManager::AudioDeviceSetup juceSetup = manager.getAudioDeviceSetup();
    juceSetup.inputDeviceName = {};
    juceSetup.useDefaultInputChannels = false;
    juceSetup.inputChannels.clear();
    juceSetup.useDefaultOutputChannels = true;
    if (!setup.outputDeviceName.empty())
        juceSetup.outputDeviceName = toJuce(setup.outputDeviceName);
    if (setup.sampleRate > 0.0)
        juceSetup.sampleRate = setup.sampleRate;
    if (setup.bufferSize > 0)
        juceSetup.bufferSize = setup.bufferSize;

    const juce::String error = manager.setAudioDeviceSetup(juceSetup, true);
    if (error.isNotEmpty())
        return makeError(ErrorCode::DeviceError, fromJuce(error));

    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return makeError(ErrorCode::DeviceError, "no audio output device could be opened");

    logging::info("audio", "device opened",
                  {{"type", fromJuce(device->getTypeName())},
                   {"device", fromJuce(device->getName())},
                   {"sampleRate", device->getCurrentSampleRate()},
                   {"bufferSize", device->getCurrentBufferSizeSamples()},
                   {"outputLatency", device->getOutputLatencyInSamples()}});
    return Status::success();
}

void AudioDeviceService::close()
{
    impl_->manager.closeAudioDevice();
    logging::info("audio", "device closed");
}

DeviceStatus AudioDeviceService::status() const
{
    DeviceStatus status;
    {
        std::lock_guard lock(impl_->errorMutex);
        status.lastError = impl_->lastError;
    }
    auto* device = impl_->manager.getCurrentAudioDevice();
    if (device == nullptr) {
        status.state = status.lastError.empty() ? DeviceState::Closed : DeviceState::Error;
        return status;
    }
    status.state = device->isPlaying() ? DeviceState::Running : (status.lastError.empty() ? DeviceState::Closed : DeviceState::Error);
    status.typeName = fromJuce(device->getTypeName());
    status.deviceName = fromJuce(device->getName());
    status.sampleRate = device->getCurrentSampleRate();
    status.bufferSize = device->getCurrentBufferSizeSamples();
    status.outputLatencySamples = device->getOutputLatencyInSamples();
    status.driverXruns = device->getXRunCount();
    return status;
}

AudioDeviceSetup AudioDeviceService::currentSetup() const
{
    AudioDeviceSetup setup;
    auto* device = impl_->manager.getCurrentAudioDevice();
    if (device == nullptr)
        return setup;
    setup.typeName = fromJuce(device->getTypeName());
    setup.outputDeviceName = fromJuce(device->getName());
    setup.sampleRate = device->getCurrentSampleRate();
    setup.bufferSize = device->getCurrentBufferSizeSamples();
    return setup;
}

} // namespace daw
