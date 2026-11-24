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

void storeMin(std::atomic<float>& target, float value) noexcept
{
    float current = target.load(std::memory_order_relaxed);
    while (value < current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void storeMax(std::atomic<double>& target, double value) noexcept
{
    double current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

// Adds a track's clips (clip gain applied) for frames [start, start + count)
// into a stereo buffer. Mono media feeds both channels.
void mixClips(const TrackPlayback& track, float masterGain, std::int64_t start, int count, float* left,
              float* right) noexcept
{
    const std::int64_t end = start + count;
    for (const ClipPlayback& clip : track.clips) {
        if (clip.timelineStart >= end)
            break; // clips are sorted
        const std::int64_t clipEnd = clip.timelineStart + clip.length;
        if (clipEnd <= start || clip.asset == nullptr)
            continue;
        const std::int64_t from = std::max(start, clip.timelineStart);
        const std::int64_t to = std::min(end, clipEnd);
        const std::int64_t source = clip.sourceStart + (from - clip.timelineStart);
        const AudioAsset& asset = *clip.asset;
        const std::int64_t available = asset.numFrames() - source;
        if (source < 0 || available <= 0)
            continue;
        const int frames = static_cast<int>(std::min(to - from, available));
        const int offset = static_cast<int>(from - start);
        const float gain = clip.gain * masterGain;
        const int channels = asset.numChannels();
        float* out[2] = {left + offset, right + offset};
        for (int c = 0; c < 2; ++c) {
            const float* in = asset.channelData(channels == 1 ? 0 : std::min(c, channels - 1)) + source;
            for (int i = 0; i < frames; ++i)
                out[c][i] += in[i] * gain;
        }
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
    fadeFrames_ = std::max(1, static_cast<int>(std::lround(rate * kDeclickSeconds)));
    fadeLevel_ = config.declick ? 0.0f : 1.0f; // a declicked engine always starts by fading in
    fadeTarget_ = 1.0f;
    pendingStop_ = pendingSeek_ = false;
    cleanupSlots_.resize(static_cast<std::size_t>(std::clamp(config.maxCleanupTracks, 0, static_cast<int>(kMaxCleanupTracks))));
    for (CleanupSlot& slot : cleanupSlots_) {
        slot.key = 0;
        slot.nextFrame = -1;
        slot.chain.prepare(rate);
    }
    for (std::size_t s = 0; s < kMaxCleanupTracks; ++s) {
        slotKeys_[s].store(0, std::memory_order_relaxed);
        slotReduction_[s].store(0.0f, std::memory_order_relaxed);
    }
    cleanupSlotOf_.fill(kNoSlot);
    cleanScratchFrames_ = std::max(config.maxBlockSize, 256);
    cleanScratch_.assign(static_cast<std::size_t>(cleanScratchFrames_) * 2, 0.0f);
    lastAdoptedVersion_ = 0; // re-adopt the current snapshot with the new slots
    loudness_.prepare(rate, config.meterLoudness ? std::clamp(config.numOutputChannels, 1, 2) : 0);
    publishLoudness();
    master_.prepare(rate);
    masterNextFrame_ = -1;
    masterReduction_.store(0.0f, std::memory_order_relaxed);
}

void AudioEngine::beginProcessing()
{
    lastCallbackTicks_ = 0;
    processing_.store(true, std::memory_order_release);
}

void AudioEngine::endProcessing()
{
    processing_.store(false, std::memory_order_release);
    // The device has stopped calling process(): apply anything still queued,
    // including a stop or seek that was waiting for its fade-out.
    drainCommands();
    flushPendingTransport();
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
        applyCommand(command);
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

float AudioEngine::takeCleanupReductionDb(std::uint64_t trackKey) noexcept
{
    if (trackKey == 0)
        return 0.0f;
    for (std::size_t s = 0; s < kMaxCleanupTracks; ++s)
        if (slotKeys_[s].load(std::memory_order_acquire) == trackKey)
            return slotReduction_[s].exchange(0.0f, std::memory_order_relaxed);
    return 0.0f;
}

LoudnessReading AudioEngine::loudness() const noexcept
{
    LoudnessReading r;
    r.momentaryLufs = loudMomentary_.load(std::memory_order_relaxed);
    r.shortTermLufs = loudShortTerm_.load(std::memory_order_relaxed);
    r.integratedLufs = loudIntegrated_.load(std::memory_order_relaxed);
    r.loudnessRangeLu = loudRange_.load(std::memory_order_relaxed);
    r.truePeakDbtp = loudTruePeak_.load(std::memory_order_relaxed);
    r.maxMomentaryLufs = loudMaxMomentary_.load(std::memory_order_relaxed);
    r.seconds = loudSeconds_.load(std::memory_order_relaxed);
    return r;
}

void AudioEngine::resetLoudness()
{
    if (!processing_.load(std::memory_order_acquire)) {
        loudness_.reset();
        publishLoudness();
        return;
    }
    loudnessResetRequested_.store(true, std::memory_order_release);
}

void AudioEngine::publishLoudness() noexcept
{
    const LoudnessReading& r = loudness_.reading();
    loudMomentary_.store(r.momentaryLufs, std::memory_order_relaxed);
    loudShortTerm_.store(r.shortTermLufs, std::memory_order_relaxed);
    loudIntegrated_.store(r.integratedLufs, std::memory_order_relaxed);
    loudRange_.store(r.loudnessRangeLu, std::memory_order_relaxed);
    loudTruePeak_.store(r.truePeakDbtp, std::memory_order_relaxed);
    loudMaxMomentary_.store(r.maxMomentaryLufs, std::memory_order_relaxed);
    loudSeconds_.store(r.seconds, std::memory_order_relaxed);
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

void AudioEngine::applyCommand(const TransportCommand& command) noexcept
{
    transport_.apply(command);
    if (command.type == TransportCommandType::Seek) {
        // A new position starts a new loudness measurement.
        loudness_.reset();
        publishLoudness();
    }
}

void AudioEngine::drainCommands() noexcept
{
    const bool declick = config_.declick && processing_.load(std::memory_order_acquire);
    TransportCommand command;
    while (commands_.pop(command)) {
        if (declick)
            handleDeclicked(command);
        else
            applyCommand(command);
    }
}

void AudioEngine::handleDeclicked(const TransportCommand& command) noexcept
{
    const bool audible = transport_.playing();
    switch (command.type) {
    case TransportCommandType::Play:
        if (pendingStop_) {
            pendingStop_ = false; // resumed during the fade-out: fade back in (after a pending seek, if any)
            if (!pendingSeek_)
                fadeTarget_ = 1.0f;
        } else if (!audible) {
            applyCommand(command);
            fadeLevel_ = 0.0f;
            fadeTarget_ = 1.0f;
        }
        break;
    case TransportCommandType::Stop:
        if (!audible) {
            applyCommand(command);
            break;
        }
        if (!pendingStop_) {
            pendingStop_ = true;
            pendingStopAt_ = pendingSeek_ ? pendingSeekTo_ : transport_.playhead();
        }
        fadeTarget_ = 0.0f;
        break;
    case TransportCommandType::Seek:
        if (!audible) {
            applyCommand(command);
            break;
        }
        pendingSeek_ = true;
        pendingSeekTo_ = std::max<std::int64_t>(0, command.value);
        if (pendingStop_)
            pendingStopAt_ = pendingSeekTo_;
        fadeTarget_ = 0.0f;
        break;
    default:
        applyCommand(command);
        break;
    }
}

void AudioEngine::applyFade(float* const* outputs, int numChannels, int numFrames) noexcept
{
    if (fadeLevel_ == 1.0f && fadeTarget_ == 1.0f)
        return;
    const float step = 1.0f / static_cast<float>(fadeFrames_);
    for (int i = 0; i < numFrames; ++i) {
        if (fadeLevel_ < fadeTarget_)
            fadeLevel_ = std::min(fadeTarget_, fadeLevel_ + step);
        else if (fadeLevel_ > fadeTarget_)
            fadeLevel_ = std::max(fadeTarget_, fadeLevel_ - step);
        for (int c = 0; c < numChannels; ++c)
            if (outputs[c] != nullptr)
                outputs[c][i] *= fadeLevel_;
    }
}

void AudioEngine::completeFadeOut() noexcept
{
    if (pendingSeek_)
        applyCommand({TransportCommandType::Seek, pendingSeekTo_});
    if (pendingStop_)
        transport_.stopAt(pendingStopAt_);
    else
        fadeTarget_ = 1.0f; // a seek during playback fades back in at the new position
    pendingSeek_ = pendingStop_ = false;
}

void AudioEngine::flushPendingTransport() noexcept
{
    if (pendingSeek_ || pendingStop_)
        completeFadeOut();
    fadeLevel_ = config_.declick ? 0.0f : 1.0f;
    fadeTarget_ = 1.0f;
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
    if (loudnessResetRequested_.exchange(false, std::memory_order_acq_rel)) {
        loudness_.reset();
        publishLoudness();
    }
    const PlaybackSnapshot* snapshot = snapshots_.acquire();
    if (snapshot != nullptr && snapshot->version != lastAdoptedVersion_) {
        lastAdoptedVersion_ = snapshot->version;
        mismatchReported_ = false;
        adoptMix(*snapshot);
        pushEvent(RtEventCode::SnapshotAdopted, static_cast<std::int64_t>(snapshot->version));
    }

    const bool wasPlaying = transport_.playing();
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
        if (config_.masterChain && numChannels > 0 && outputs[0] != nullptr) {
            // The limiter's delay holds the audio just before the playhead:
            // after a jump it belongs somewhere else, so it is cleared.
            const std::int64_t at = transport_.playhead();
            if (at != masterNextFrame_)
                master_.reset();
            float* right = numChannels > 1 && outputs[1] != nullptr ? outputs[1] : outputs[0];
            master_.process(outputs[0], right, numFrames);
            masterNextFrame_ = at + numFrames;
            storeMin(masterReduction_, master_.takeMinGainDb());
        }
        applyFade(outputs, numChannels, numFrames);
        transport_.advance(numFrames);
        if (fadeLevel_ == 0.0f && fadeTarget_ == 0.0f)
            completeFadeOut();
        if (snapshot != nullptr && transport_.stopAtEnd() && transport_.playing() && transport_.playhead() >= snapshot->endFrame) {
            transport_.stopAt(snapshot->endFrame);
            pushEvent(RtEventCode::TransportStoppedAtEnd, snapshot->endFrame);
        }
    }
    if (config_.declick && !transport_.playing()) {
        // Whatever stopped playback, the next Play starts from silence.
        fadeLevel_ = 0.0f;
        fadeTarget_ = 1.0f;
        pendingStop_ = pendingSeek_ = false;
    }

    // Loudness: only what is played is measured (no silence while paused).
    if (wasPlaying && numChannels > 0 && outputs[0] != nullptr) {
        const float* meterInputs[2] = {outputs[0], numChannels > 1 && outputs[1] != nullptr ? outputs[1] : outputs[0]};
        if (loudness_.process(meterInputs, std::min(numChannels, 2), numFrames))
            publishLoudness();
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
    master_.setParams(snapshot.master);
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
    adoptCleanup(snapshot);
}

void AudioEngine::adoptCleanup(const PlaybackSnapshot& snapshot) noexcept
{
    // Tracks keep their slot (and filter memory) by key; slots of tracks that
    // are gone or no longer processed are freed, new ones start primed.
    slotClaimed_.fill(false);
    const std::size_t count = std::min(snapshot.tracks.size(), kMaxRampedTracks);
    const std::size_t slots = cleanupSlots_.size();
    for (std::size_t i = 0; i < count; ++i) {
        cleanupSlotOf_[i] = kNoSlot;
        const TrackPlayback& track = snapshot.tracks[i];
        if (track.key == 0 || !track.cleanup.active())
            continue;
        for (std::size_t s = 0; s < slots; ++s)
            if (!slotClaimed_[s] && cleanupSlots_[s].key == track.key) {
                cleanupSlotOf_[i] = static_cast<int>(s);
                slotClaimed_[s] = true;
                break;
            }
    }
    for (std::size_t s = 0; s < slots; ++s)
        if (!slotClaimed_[s]) {
            cleanupSlots_[s].key = 0;
            slotKeys_[s].store(0, std::memory_order_release);
        }
    for (std::size_t i = 0; i < count; ++i) {
        const TrackPlayback& track = snapshot.tracks[i];
        if (cleanupSlotOf_[i] == kNoSlot && track.key != 0 && track.cleanup.active()) {
            for (std::size_t s = 0; s < slots; ++s)
                if (cleanupSlots_[s].key == 0) {
                    CleanupSlot& slot = cleanupSlots_[s];
                    slot.key = track.key;
                    slot.nextFrame = -1;
                    slotReduction_[s].store(0.0f, std::memory_order_relaxed);
                    slotKeys_[s].store(track.key, std::memory_order_release);
                    slotClaimed_[s] = true;
                    cleanupSlotOf_[i] = static_cast<int>(s);
                    break;
                }
        }
        if (cleanupSlotOf_[i] != kNoSlot) {
            CleanupSlot& slot = cleanupSlots_[static_cast<std::size_t>(cleanupSlotOf_[i])];
            if (!(slot.chain.params() == track.cleanup)) {
                if (CleanupChain::needsRestart(slot.chain.params(), track.cleanup))
                    slot.nextFrame = -1; // re-primed on the next block
                slot.chain.setParams(track.cleanup);
            }
        }
    }
}

void AudioEngine::renderCleanTrack(const TrackPlayback& track, float masterGain, const MixRamp& ramp, std::size_t slotIndex,
                                   float* const* outputs, int numChannels, std::int64_t position, int numFrames) noexcept
{
    CleanupSlot& slot = cleanupSlots_[slotIndex];
    CleanupChain& chain = slot.chain;
    const int latency = chain.latency();
    const int capacity = cleanScratchFrames_;
    float* left = cleanScratch_.data();
    float* right = left + capacity;

    // The chain reads `latency` frames ahead. After a seek, a loop, or a gap in
    // rendering (the track was silent), restart it and feed the frames it
    // needs before `position` is heard.
    if (slot.nextFrame != position) {
        chain.reset();
        for (int done = 0; done < latency;) {
            const int n = std::min(latency - done, capacity);
            std::fill_n(left, n, 0.0f);
            std::fill_n(right, n, 0.0f);
            mixClips(track, masterGain, position + done, n, left, right);
            chain.process(left, right, n);
            done += n;
        }
        chain.takeMinGainDb(); // priming is not heard
    }

    const int remaining = ramp.remaining();
    const int renderChannels = std::min(numChannels, 2);
    for (int done = 0; done < numFrames;) {
        const int n = std::min(numFrames - done, capacity);
        std::fill_n(left, n, 0.0f);
        std::fill_n(right, n, 0.0f);
        mixClips(track, masterGain, position + latency + done, n, left, right);
        chain.process(left, right, n);

        const int ramped = std::clamp(remaining - done, 0, n);
        const float* processed[2] = {left, right};
        for (int c = 0; c < renderChannels; ++c) {
            if (outputs[c] == nullptr)
                continue;
            const float* in = processed[c];
            float* out = outputs[c] + done;
            for (int i = 0; i < ramped; ++i)
                out[i] += in[i] * (ramp.start[c] + ramp.step[c] * static_cast<float>(ramp.position + done + i + 1));
            const float g = ramp.target[c];
            for (int i = ramped; i < n; ++i)
                out[i] += in[i] * g;
        }
        done += n;
    }
    slot.nextFrame = position + numFrames;
    storeMin(slotReduction_[slotIndex], chain.takeMinGainDb());
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
        const int slot = t < rampCount_ ? cleanupSlotOf_[t] : kNoSlot;
        if (!silent && slot != kNoSlot) {
            renderCleanTrack(track, masterGain, ramp, static_cast<std::size_t>(slot), outputs, numChannels, position,
                             numFrames);
        } else if (!silent) {
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
