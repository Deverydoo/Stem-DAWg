// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/engine/AudioEngine.h>

#include <daw/core/Denormals.h>
#include <daw/core/RtThread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace daw {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t nowTicks() noexcept
{
    return Clock::now().time_since_epoch().count();
}

double ticksToSeconds(std::int64_t ticks) noexcept
{
    return static_cast<double>(ticks) * static_cast<double>(Clock::period::num) / static_cast<double>(Clock::period::den);
}

void storeMax(std::atomic<float>& target, float value) noexcept
{
    float current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void storeMax(std::atomic<double>& target, double value) noexcept
{
    double current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() = default;

void AudioEngine::prepare(const EngineConfig& config)
{
    config_ = config;
    lastCallbackTicks_ = 0;
    mismatchReported_ = false;
    const double rate = config.sampleRate > 0.0 ? config.sampleRate : 48000.0;
    rampFrames_ = std::max(1, static_cast<int>(std::lround(rate * 0.010))); // 10 ms gain ramps
}

void AudioEngine::beginProcessing()
{
    lastCallbackTicks_ = 0;
    processing_.store(true, std::memory_order_release);
}

void AudioEngine::endProcessing()
{
    processing_.store(false, std::memory_order_release);
    // The device has stopped calling process(): apply anything still queued.
    drainCommands();
    publishTransport();
}

bool AudioEngine::play()
{
    return sendCommand({TransportCommandType::Play, 0});
}

bool AudioEngine::stop()
{
    return sendCommand({TransportCommandType::Stop, 0});
}

bool AudioEngine::seek(std::int64_t frame)
{
    return sendCommand({TransportCommandType::Seek, frame});
}

bool AudioEngine::setStopAtEnd(bool enabled)
{
    return sendCommand({TransportCommandType::SetStopAtEnd, enabled ? 1 : 0});
}

bool AudioEngine::sendCommand(const TransportCommand& command)
{
    if (!processing_.load(std::memory_order_acquire)) {
        transport_.apply(command);
        publishTransport();
        return true;
    }
    if (!commands_.push(command)) {
        droppedCommands_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void AudioEngine::publishSnapshot(std::unique_ptr<PlaybackSnapshot> snapshot)
{
    snapshots_.publish(std::move(snapshot));
}

std::size_t AudioEngine::collectGarbage()
{
    return snapshots_.collectGarbage();
}

bool AudioEngine::popEvent(RtEvent& event)
{
    return events_.pop(event);
}

TransportState AudioEngine::transportState() const noexcept
{
    return {publishedPlayhead_.load(std::memory_order_acquire), publishedPlaying_.load(std::memory_order_acquire)};
}

MeterPeaks AudioEngine::takeMeterPeaks() noexcept
{
    return {meterLeft_.exchange(0.0f, std::memory_order_relaxed), meterRight_.exchange(0.0f, std::memory_order_relaxed)};
}

EngineDiagnostics AudioEngine::diagnostics() const noexcept
{
    EngineDiagnostics d;
    d.callbacks = callbacks_.load(std::memory_order_relaxed);
    d.deadlineMisses = deadlineMisses_.load(std::memory_order_relaxed);
    d.callbackGaps = callbackGaps_.load(std::memory_order_relaxed);
    d.droppedCommands = droppedCommands_.load(std::memory_order_relaxed);
    d.droppedEvents = droppedEvents_.load(std::memory_order_relaxed);
    d.rateMismatchBlocks = rateMismatchBlocks_.load(std::memory_order_relaxed);
    d.snapshotSwaps = snapshots_.swapCount();
    d.snapshotLeaks = snapshots_.leakCount();
    d.lastLoad = lastLoad_.load(std::memory_order_relaxed);
    d.peakLoad = peakLoad_.load(std::memory_order_relaxed);
    return d;
}

void AudioEngine::drainCommands() noexcept
{
    TransportCommand command;
    while (commands_.pop(command))
        transport_.apply(command);
}

void AudioEngine::publishTransport() noexcept
{
    publishedPlayhead_.store(transport_.playhead(), std::memory_order_release);
    publishedPlaying_.store(transport_.playing(), std::memory_order_release);
}

void AudioEngine::pushEvent(RtEventCode code, std::int64_t a, std::int64_t b) noexcept
{
    if (!events_.push(RtEvent{code, a, b}))
        droppedEvents_.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::process(float* const* outputs, int numChannels, int numFrames) noexcept
{
    ScopedFlushDenormals noDenormals;
    rt::ScopedAudioThread audioThreadMarker;

    const std::int64_t startTicks = nowTicks();
    const double sampleRate = config_.sampleRate > 0.0 ? config_.sampleRate : 48000.0;
    const double budgetSeconds = static_cast<double>(numFrames) / sampleRate;

    if (lastCallbackTicks_ != 0 && numFrames > 0) {
        const double gap = ticksToSeconds(startTicks - lastCallbackTicks_);
        if (gap > 2.0 * budgetSeconds + 0.002) {
            callbackGaps_.fetch_add(1, std::memory_order_relaxed);
            pushEvent(RtEventCode::CallbackGap, static_cast<std::int64_t>(gap * 1e6),
                      static_cast<std::int64_t>(budgetSeconds * 1e6));
        }
    }
    lastCallbackTicks_ = startTicks;
    callbacks_.fetch_add(1, std::memory_order_relaxed);

    for (int c = 0; c < numChannels; ++c)
        if (outputs[c] != nullptr)
            std::memset(outputs[c], 0, sizeof(float) * static_cast<std::size_t>(std::max(numFrames, 0)));

    drainCommands();
    const PlaybackSnapshot* snapshot = snapshots_.acquire();
    if (snapshot != nullptr && snapshot->version != lastAdoptedVersion_) {
        lastAdoptedVersion_ = snapshot->version;
        mismatchReported_ = false;
        adoptMix(*snapshot);
        pushEvent(RtEventCode::SnapshotAdopted, static_cast<std::int64_t>(snapshot->version));
    }

    if (transport_.playing() && numFrames > 0) {
        if (snapshot != nullptr) {
            if (snapshot->sampleRate == config_.sampleRate) {
                render(*snapshot, outputs, numChannels, transport_.playhead(), numFrames);
            } else {
                rateMismatchBlocks_.fetch_add(1, std::memory_order_relaxed);
                if (!mismatchReported_) {
                    mismatchReported_ = true;
                    pushEvent(RtEventCode::SampleRateMismatch, static_cast<std::int64_t>(snapshot->sampleRate),
                              static_cast<std::int64_t>(config_.sampleRate));
                }
            }
        }
        transport_.advance(numFrames);
        if (snapshot != nullptr && transport_.stopAtEnd() && transport_.playhead() >= snapshot->endFrame) {
            transport_.stopAt(snapshot->endFrame);
            pushEvent(RtEventCode::TransportStoppedAtEnd, snapshot->endFrame);
        }
    }

    // Metering (first two channels).
    for (int c = 0; c < std::min(numChannels, 2); ++c) {
        if (outputs[c] == nullptr)
            continue;
        float peak = 0.0f;
        for (int i = 0; i < numFrames; ++i)
            peak = std::max(peak, std::abs(outputs[c][i]));
        storeMax(c == 0 ? meterLeft_ : meterRight_, peak);
    }

    publishTransport();

    const double elapsed = ticksToSeconds(nowTicks() - startTicks);
    const double load = budgetSeconds > 0.0 ? elapsed / budgetSeconds : 0.0;
    lastLoad_.store(load, std::memory_order_relaxed);
    storeMax(peakLoad_, load);
    if (load > 1.0) {
        deadlineMisses_.fetch_add(1, std::memory_order_relaxed);
        pushEvent(RtEventCode::DeadlineMiss, static_cast<std::int64_t>(elapsed * 1e6),
                  static_cast<std::int64_t>(budgetSeconds * 1e6));
    }
}

void AudioEngine::adoptMix(const PlaybackSnapshot& snapshot) noexcept
{
    // Carry each track's current gain into the new snapshot so gain changes
    // ramp instead of jumping. While stopped there is nothing to smooth.
    const std::size_t previous = rampCount_;
    std::copy_n(ramps_.begin(), previous, rampScratch_.begin());
    const std::size_t count = std::min(snapshot.tracks.size(), kMaxRampedTracks);
    const bool smooth = transport_.playing();

    for (std::size_t i = 0; i < count; ++i) {
        const TrackPlayback& track = snapshot.tracks[i];
        const MixRamp* old = nullptr;
        if (track.key != 0) {
            if (i < previous && rampScratch_[i].key == track.key) {
                old = &rampScratch_[i];
            } else {
                for (std::size_t j = 0; j < previous; ++j)
                    if (rampScratch_[j].key == track.key) {
                        old = &rampScratch_[j];
                        break;
                    }
            }
        }

        MixRamp next;
        next.key = track.key;
        next.target[0] = track.gainLeft;
        next.target[1] = track.gainRight;
        if (old != nullptr && smooth && (old->current(0) != next.target[0] || old->current(1) != next.target[1])) {
            for (int c = 0; c < 2; ++c) {
                next.start[c] = old->current(c);
                next.step[c] = (next.target[c] - next.start[c]) / static_cast<float>(rampFrames_);
            }
            next.length = rampFrames_;
        }
        ramps_[i] = next;
    }
    rampCount_ = count;
}

void AudioEngine::render(const PlaybackSnapshot& snapshot, float* const* outputs, int numChannels,
                         std::int64_t position, int numFrames) noexcept
{
    const std::int64_t blockEnd = position + numFrames;
    const int renderChannels = std::min(numChannels, 2);
    const float masterGain = snapshot.masterGain;

    for (std::size_t t = 0; t < snapshot.tracks.size(); ++t) {
        const TrackPlayback& track = snapshot.tracks[t];
        MixRamp unramped;
        unramped.target[0] = track.gainLeft;
        unramped.target[1] = track.gainRight;
        MixRamp& ramp = t < rampCount_ ? ramps_[t] : unramped;
        const int remaining = ramp.remaining();

        const bool silent = remaining == 0 && ramp.target[0] == 0.0f && ramp.target[1] == 0.0f;
        if (!silent) {
            for (const ClipPlayback& clip : track.clips) {
                if (clip.timelineStart >= blockEnd)
                    break; // clips are sorted
                const std::int64_t clipEnd = clip.timelineStart + clip.length;
                if (clipEnd <= position || clip.asset == nullptr)
                    continue;

                const std::int64_t start = std::max(position, clip.timelineStart);
                const std::int64_t end = std::min(blockEnd, clipEnd);
                const std::int64_t source = clip.sourceStart + (start - clip.timelineStart);
                const AudioAsset& asset = *clip.asset;
                const std::int64_t available = asset.numFrames() - source;
                if (source < 0 || available <= 0)
                    continue;
                const int count = static_cast<int>(std::min(end - start, available));
                const int offset = static_cast<int>(start - position);
                const float clipGain = clip.gain * masterGain;
                const int assetChannels = asset.numChannels();
                // Frames of this segment that are still inside the gain ramp.
                const int ramped = std::clamp(remaining - offset, 0, count);

                for (int c = 0; c < renderChannels; ++c) {
                    const int sourceChannel = assetChannels == 1 ? 0 : c;
                    if (sourceChannel >= assetChannels || outputs[c] == nullptr)
                        continue;
                    const float* in = asset.channelData(sourceChannel) + source;
                    float* out = outputs[c] + offset;
                    for (int i = 0; i < ramped; ++i) {
                        const float g = ramp.start[c] + ramp.step[c] * static_cast<float>(ramp.position + offset + i + 1);
                        out[i] += in[i] * (clipGain * g);
                    }
                    const float g = clipGain * ramp.target[c];
                    for (int i = ramped; i < count; ++i)
                        out[i] += in[i] * g;
                }
            }
        }

        if (remaining > numFrames)
            ramp.position += numFrames;
        else
            ramp.position = ramp.length = 0; // reached the target
    }
}

} // namespace daw
