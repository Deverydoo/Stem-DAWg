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
//  * transportState()/takeMeterPeaks()/loudness()/diagnostics() are lock-free
//    reads that are safe from any thread.
//  * While processing is active, commands are queued and applied at the start
//    of the next block (block-accurate). While stopped they apply immediately.
//  * With EngineConfig::declick, Stop and Seek during playback first fade the
//    output out over kDeclickSeconds and take effect when it is silent; Play
//    and a completed Seek fade back in. Stop keeps the position it was asked
//    at. Off by default so offline renders stay sample-exact.

#include <daw/core/RtEvent.h>
#include <daw/core/SnapshotExchange.h>
#include <daw/core/SpscQueue.h>
#include <daw/dsp/CleanupChain.h>
#include <daw/dsp/LoudnessMeter.h>
#include <daw/engine/PlaybackSnapshot.h>
#include <daw/engine/Transport.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace daw {

struct EngineConfig {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int numOutputChannels = 2;
    bool meterLoudness = true; // live loudness metering (offline renders that measure themselves turn it off)
    bool declick = false;      // fade out/in around stop, seek and play (the live device turns it on)
    // Tracks that can run a clean-up chain at once (each chain preallocates
    // ~140 KB of FFT and delay buffers). Further tracks play dry. At most 256.
    int maxCleanupTracks = 128;
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
    static constexpr double kDeclickSeconds = 0.005;

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
    /// Live loudness of the output while playing (BS.1770 / EBU R 128). The
    /// integrated value, range and true peak accumulate from the last seek or
    /// resetLoudness(); pausing keeps them. Values update every 100 ms.
    LoudnessReading loudness() const noexcept;
    /// Restarts the loudness measurement (control thread).
    void resetLoudness();
    /// Deepest expander gain reduction (dB, <= 0) of the track with this
    /// snapshot key since the last call; 0 when it has no running chain.
    float takeCleanupReductionDb(std::uint64_t trackKey) noexcept;
    EngineDiagnostics diagnostics() const noexcept;

    // ---- real-time ---------------------------------------------------------
    /// Renders `numFrames` frames into `outputs` (overwritten, not accumulated).
    void process(float* const* outputs, int numChannels, int numFrames) noexcept;

private:
    bool sendCommand(const TransportCommand& command);
    void applyCommand(const TransportCommand& command) noexcept;
    void drainCommands() noexcept;
    void handleDeclicked(const TransportCommand& command) noexcept;
    void applyFade(float* const* outputs, int numChannels, int numFrames) noexcept;
    void completeFadeOut() noexcept;
    void flushPendingTransport() noexcept;
    void publishLoudness() noexcept;
    void publishTransport() noexcept;
    void pushEvent(RtEventCode code, std::int64_t a = 0, std::int64_t b = 0) noexcept;
    void render(const PlaybackSnapshot& snapshot, float* const* outputs, int numChannels, std::int64_t position,
                int numFrames) noexcept;
    void adoptMix(const PlaybackSnapshot& snapshot) noexcept;
    void adoptCleanup(const PlaybackSnapshot& snapshot) noexcept;
    struct MixRamp;
    struct CleanupSlot;
    void renderCleanTrack(const TrackPlayback& track, float masterGain, const MixRamp& ramp, std::size_t slotIndex,
                          float* const* outputs, int numChannels, std::int64_t position, int numFrames) noexcept;

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

    // Clean-up chain state per track (audio-thread private after prepare).
    // Slots are claimed by track key when a snapshot is adopted, so a track
    // keeps its filter memory while its settings change.
    struct CleanupSlot {
        std::uint64_t key = 0;     // 0 = free
        std::int64_t nextFrame = -1; // output frame the chain continues from; anything else re-primes it
        CleanupChain chain;
    };
    static constexpr std::size_t kMaxCleanupTracks = kMaxRampedTracks; // upper bound for EngineConfig::maxCleanupTracks
    static constexpr int kNoSlot = -1;

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
    std::atomic<bool> loudnessResetRequested_{false};
    std::atomic<double> loudMomentary_{kLoudnessFloor};
    std::atomic<double> loudShortTerm_{kLoudnessFloor};
    std::atomic<double> loudIntegrated_{kLoudnessFloor};
    std::atomic<double> loudRange_{0.0};
    std::atomic<double> loudTruePeak_{kLoudnessFloor};
    std::atomic<double> loudMaxMomentary_{kLoudnessFloor};
    std::atomic<double> loudSeconds_{0.0};

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
    std::vector<CleanupSlot> cleanupSlots_;           // EngineConfig::maxCleanupTracks, allocated in prepare()
    std::array<int, kMaxRampedTracks> cleanupSlotOf_{}; // per snapshot track: slot index or kNoSlot
    std::array<bool, kMaxCleanupTracks> slotClaimed_{};
    std::array<std::atomic<std::uint64_t>, kMaxCleanupTracks> slotKeys_{};  // published copy of CleanupSlot::key
    std::array<std::atomic<float>, kMaxCleanupTracks> slotReduction_{};    // dB, min since the last take
    std::vector<float> cleanScratch_;                 // 2 x cleanScratchFrames_, allocated in prepare()
    int cleanScratchFrames_ = 0;
    LoudnessMeter loudness_; // prepared in prepare(), then audio-thread owned while processing

    // Declick (audio-thread private while processing).
    int fadeFrames_ = 240;
    float fadeLevel_ = 1.0f;  // output gain now
    float fadeTarget_ = 1.0f; // 0 while fading out for a pending stop/seek
    bool pendingStop_ = false;
    bool pendingSeek_ = false;
    std::int64_t pendingStopAt_ = 0;
    std::int64_t pendingSeekTo_ = 0;
};

} // namespace daw
