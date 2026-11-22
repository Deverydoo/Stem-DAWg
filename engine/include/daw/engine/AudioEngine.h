// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The real-time audio engine. Device-independent: the device layer (or a test,
// or the offline renderer) calls process() with output buffers.
//
// Threading contract
// ------------------
//  * process() runs on the real-time thread. It never allocates, frees, locks,
//    blocks, logs or performs I/O (verified by tests/engine allocation guard).
//  * Control methods (prepare/begin/end/play/stop/seek/publishSnapshot/
//    collectGarbage/popEvent) are called from ONE control thread (the Qt main
//    thread in the app). Device start/stop must also happen on that thread.
//  * transportState()/takeMeterPeaks()/diagnostics() are lock-free reads that
//    are safe from any thread.
//  * While processing is active, commands are queued and applied at the start
//    of the next block (block-accurate). While stopped they apply immediately.

#include <daw/core/RtEvent.h>
#include <daw/core/SnapshotExchange.h>
#include <daw/core/SpscQueue.h>
#include <daw/engine/PlaybackSnapshot.h>
#include <daw/engine/Transport.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace daw {

struct EngineConfig {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int numOutputChannels = 2;
};

struct MeterPeaks {
    float left = 0.0f;
    float right = 0.0f;
};

struct EngineDiagnostics {
    std::uint64_t callbacks = 0;
    std::uint64_t deadlineMisses = 0;     // block took longer than its real-time budget
    std::uint64_t callbackGaps = 0;       // interval between callbacks > 2x block duration
    std::uint64_t droppedCommands = 0;    // control command queue full
    std::uint64_t droppedEvents = 0;      // RT event queue full
    std::uint64_t rateMismatchBlocks = 0; // snapshot rate != engine rate
    std::uint64_t snapshotSwaps = 0;
    std::uint64_t snapshotLeaks = 0;
    double lastLoad = 0.0;                // last block processing time / budget
    double peakLoad = 0.0;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ---- lifecycle (control thread) ----------------------------------------
    void prepare(const EngineConfig& config);
    void beginProcessing();
    void endProcessing();
    bool isProcessing() const noexcept { return processing_.load(std::memory_order_acquire); }
    const EngineConfig& config() const noexcept { return config_; }

    // ---- transport (control thread) ----------------------------------------
    bool play();
    bool stop();
    bool seek(std::int64_t frame);
    bool setStopAtEnd(bool enabled);

    // ---- content (control thread) ------------------------------------------
    void publishSnapshot(std::unique_ptr<PlaybackSnapshot> snapshot);
    /// Frees snapshots retired by the audio thread. Call periodically (~30 Hz).
    std::size_t collectGarbage();
    /// Drains one event emitted by the audio thread.
    bool popEvent(RtEvent& event);

    // ---- observation (any thread, lock-free) -------------------------------
    TransportState transportState() const noexcept;
    /// Returns and resets the per-channel absolute peaks since the last call.
    MeterPeaks takeMeterPeaks() noexcept;
    EngineDiagnostics diagnostics() const noexcept;

    // ---- real-time ---------------------------------------------------------
    /// Renders `numFrames` frames into `outputs` (overwritten, not accumulated).
    void process(float* const* outputs, int numChannels, int numFrames) noexcept;

private:
    bool sendCommand(const TransportCommand& command);
    void drainCommands() noexcept;
    void publishTransport() noexcept;
    void pushEvent(RtEventCode code, std::int64_t a = 0, std::int64_t b = 0) noexcept;
    void render(const PlaybackSnapshot& snapshot, float* const* outputs, int numChannels, std::int64_t position,
                int numFrames) noexcept;
    void adoptMix(const PlaybackSnapshot& snapshot) noexcept;

    // Per-track gain smoothing (audio-thread private, preallocated). Index i
    // follows snapshot track i; state is matched to the previous snapshot by key.
    // Gains are computed as start + step * n from an integer frame count, so
    // ramps are bit-identical whatever the device block size.
    struct MixRamp {
        std::uint64_t key = 0;
        float start[2] = {1.0f, 1.0f};
        float target[2] = {1.0f, 1.0f};
        float step[2] = {0.0f, 0.0f};
        int position = 0; // frames already ramped
        int length = 0;   // 0 = at target
        int remaining() const noexcept { return length - position; }
        float current(int c) const noexcept
        {
            return length == 0 ? target[c] : start[c] + step[c] * static_cast<float>(position);
        }
    };
    static constexpr std::size_t kMaxRampedTracks = 256; // further tracks switch gain without a ramp

    EngineConfig config_{};
    Transport transport_; // audio-thread owned while processing

    SpscQueue<TransportCommand> commands_{256};
    SnapshotExchange<PlaybackSnapshot> snapshots_{64};
    SpscQueue<RtEvent> events_{1024};

    std::atomic<bool> processing_{false};
    std::atomic<std::int64_t> publishedPlayhead_{0};
    std::atomic<bool> publishedPlaying_{false};
    std::atomic<float> meterLeft_{0.0f};
    std::atomic<float> meterRight_{0.0f};

    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> deadlineMisses_{0};
    std::atomic<std::uint64_t> callbackGaps_{0};
    std::atomic<std::uint64_t> droppedCommands_{0};
    std::atomic<std::uint64_t> droppedEvents_{0};
    std::atomic<std::uint64_t> rateMismatchBlocks_{0};
    std::atomic<double> lastLoad_{0.0};
    std::atomic<double> peakLoad_{0.0};

    // audio-thread private
    std::int64_t lastCallbackTicks_ = 0;
    std::uint64_t lastAdoptedVersion_ = 0;
    bool mismatchReported_ = false;
    int rampFrames_ = 480;
    std::size_t rampCount_ = 0;
    std::array<MixRamp, kMaxRampedTracks> ramps_{};
    std::array<MixRamp, kMaxRampedTracks> rampScratch_{};
};

} // namespace daw
