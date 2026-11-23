# Stem DAWg — Milestone 1 Technical Specification

**Engineering foundation** · Windows 11 x64 · C++20 · Qt 6 UI · JUCE audio engine · ONNX Runtime (integrated)
Status: implemented in this repository (v0.1.0). All acceptance criteria are covered by automated tests (§11).

---

## 1. Scope and acceptance criteria

Milestone 1 is a working desktop foundation, not yet a multitrack editor. Its job is to make the hard parts real: real-time audio that never waits on anything, a persistent project format that can evolve, and an AI service boundary that already does work.

| # | Component | Acceptance target | Verified by |
|---|-----------|-------------------|-------------|
| A1 | Application | Native Qt window launches on Windows 11 x64 | Manual run; startup log line `app/starting` |
| A2 | Audio devices | Enumerate device types/devices/rates/buffer sizes; open, reconfigure, close | `AudioDeviceService` (manual, needs hardware); `DeviceDialog` |
| A3 | Audio engine | Callback starts/stops without UI dependency; allocation-free | `RealtimeSafetyTests` (allocation guard), `AudioEngineTests` |
| A4 | Transport | Play, stop, seek, integer sample position | `TransportTests`, `AudioEngineTests` |
| A5 | Playback | Stereo WAV through a processing path, sample-exact | `AudioEngineTests` "sample-exact", block-size invariance regression |
| A6 | Waveforms | Cached multi-resolution peak data drives the waveform view | `PeakPyramidTests`, `PeakCacheTests`, `SessionTests` |
| A7 | Projects | Create, save, reopen a versioned project; migrations; atomic save; autosave | `SerializationTests`, `MigrationTests`, `ProjectStoreTests` |
| A8 | AI subsystem | Background analysis service, ONNX model loading, execution-provider selection | `AnalysisServiceTests`, `InferenceTests` |
| A9 | Testing | Automated transport, serialization and audio-processing tests | 90 CTest cases (§11) |
| A10 | Diagnostics | Device errors, underrun/deadline counters, structured logging | `EngineDiagnostics`, `CoreUtilityTests` (logger), diagnostics panel |

**Definition of done.** Load a WAV, see its waveform, play it, stop it, seek, save the project, reopen it, and reproduce the same playback state, while background analysis runs without blocking the UI or audio. This is automated end-to-end, without a device, in `tests/integration/DefinitionOfDoneTests.cpp`: after reopening, the rendered output is compared **bit-for-bit** against the pre-save render, at both the native rate and a resampled engine rate.

---

## 2. Platform and toolchain

| Item | Choice | Notes |
|------|--------|-------|
| OS | Windows 11 x64 (Windows 10 1809+ works) | `_WIN32_WINNT=0x0A00` |
| Compiler | MSVC 19.4x (Visual Studio 2022), C++20 | `/W4 /permissive- /utf-8`; currently 0 warnings |
| Build | CMake ≥ 3.25, presets, VS 2022 generator | Dependencies fetched at configure time |
| UI | Qt 6.5+ Widgets (built against 6.12.0) | `QT_NO_KEYWORDS`; `windeployqt` runs post-build |
| Audio / DSP | JUCE 8.0.7, non-GUI modules only | WASAPI + DirectSound; ASIO opt-in |
| AI inference | ONNX Runtime 1.22.0 (NuGet `Microsoft.ML.OnnxRuntime`, CPU) | DirectML/CUDA packages drop in via `DAW_ONNXRUNTIME_ROOT` |
| JSON | nlohmann/json 3.11.3 | Project files, analysis results, logs |
| Tests | Catch2 3.7.1 + CTest | `catch_discover_tests` (PRE_TEST) |

The automatic ONNX download uses NuGet because the GitHub release URLs were unreachable from this build machine. Bump the version with `-DDAW_ONNXRUNTIME_VERSION=…`; the C++ API calls used are long-stable.

---

## 3. Architecture

### 3.1 Layers

```
┌───────────────────────────── apps/desktop (Qt) ─────────────────────────────┐
│ MainWindow · WaveformView · DeviceDialog · DiagnosticsPanel                  │
│ EngineBridge (30 Hz state poll)   ProjectController (model, jobs, mapping)   │
└───────────────┬───────────────────────────────┬──────────────────────────────┘
                │ commands / snapshots          │ jobs / results
┌───────────────▼──────────────┐   ┌────────────▼─────────────────────────────┐
│ session                      │   │ ai                                        │
│ MediaPool · MediaLoader      │   │ AnalysisService · IAnalyzer · analyzers   │
│ SnapshotBuilder · ProjectOps │   │ InferenceBackend (ONNX) · ModelRegistry   │
└──┬──────────┬───────────┬────┘   └────────────┬─────────────────────────────┘
   │          │           │                     │
┌──▼───┐  ┌───▼────┐  ┌───▼─────┐               │
│engine│  │ media  │  │ project │               │
│(JUCE │  │(JUCE   │  │ (JSON)  │               │
│ dev) │  │ decode)│  │         │               │
└──┬───┘  └───┬────┘  └───┬─────┘               │
   └──────────┴─────┬─────┴─────────────────────┘
                ┌───▼──┐
                │ core │  Result · Ids · SpscQueue · SnapshotExchange · Logger · Files · AudioAsset
                └──────┘
```

### 3.2 Module dependency rules (enforced by CMake target linkage)

| Module | May depend on | Must not depend on | Rationale |
|--------|---------------|--------------------|-----------|
| `core` | std, nlohmann_json | JUCE, Qt, ORT | Shared vocabulary, RT primitives |
| `dsp` | core | JUCE, Qt, ORT | Signal processing shared by engine, analyzers and renders (added in M2) |
| `media` | core, JUCE (private) | Qt | Decoding/peaks run headless |
| `engine` | core, dsp, JUCE (private) | Qt, project, ai | RT code stays independent of persistence and UI |
| `project` | core | JUCE, Qt | Project files readable by tools and tests without audio stacks |
| `session` | core, media, engine, project | Qt | Glue that is fully testable headless |
| `ai` | core, dsp, ORT (private) | JUCE, Qt, engine | Inference can never reach the audio thread |
| `apps/desktop` | all + Qt | — | Only place Qt appears |

JUCE is linked `PRIVATE`, and public headers of `engine` and `media` are JUCE-free (pimpl). Qt translation units therefore never include JUCE headers, which avoids macro and keyword collisions.

JUCE is compiled once into `daw_juce` (`third_party/juce_static`). JUCE module targets inject their sources into every consumer, so `daw_juce` links them `PRIVATE` and re-exports only include directories and definitions.

### 3.3 Repository layout (Milestone 1 subset of the roadmap tree)

```
CMakeLists.txt  CMakePresets.json  cmake/  third_party/juce_static/
core/     include/daw/core/*     src/*
dsp/      include/daw/dsp/*      src/*      (added in M2)
media/    include/daw/media/*    src/*
engine/   include/daw/engine/*   src/*
project/  include/daw/project/*  src/*
session/  include/daw/session/*  src/*
ai/       include/daw/ai/*       src/*
apps/desktop/                    Qt application
tests/    core/ engine/ media/ project/ ai/ integration/ support/
docs/milestone-1/
```

Later milestones add `reconstruction/`, `plugins/` and `apps/plugin_scanner/` beside these, following the same `include/` + `src/` + `daw_add_module` pattern.

---

## 4. Threading and ownership model

### 4.1 Threads

| Thread | Owner | Runs | Never does |
|--------|-------|------|------------|
| **Qt main (control)** | `QApplication` | UI, project model, device lifecycle, engine control API, garbage collection of snapshots | Touch buffers being rendered |
| **Audio (real-time)** | JUCE device (WASAPI/DS/ASIO) | `AudioEngine::process()` only | Allocate, free, lock, block, log, file I/O, IPC, inference |
| **Media workers** (2) | `ProjectController::workers_` (`QThreadPool`) | Import, decode, resample, peak build/cache | Touch the model directly (results are marshalled back) |
| **AI workers** (`clamp(hw/4, 1, 4)`) | `AnalysisService` | Analyzer jobs, ONNX inference | Run at normal priority (they use `THREAD_PRIORITY_BELOW_NORMAL`) |
| **ORT intra-op** (≤ 2 per session) | ONNX Runtime | Kernel execution | — (capped to protect audio headroom) |
| **Log writer** | `Logger` | Formats sinks and writes the JSON-lines file | — |

### 4.2 Real-time contract

`AudioEngine::process(float* const* out, int channels, int frames) noexcept`:

1. Flush-to-zero/denormals-are-zero for the scope (`ScopedFlushDenormals`).
2. Mark the thread as the audio thread (`rt::ScopedAudioThread`). The logger then drops records instead of blocking.
3. Clear the outputs.
4. Drain transport commands (`SpscQueue<TransportCommand>`, capacity 256).
5. Adopt the newest `PlaybackSnapshot` (`SnapshotExchange::acquire`).
6. If playing and `snapshot.sampleRate == engine rate`: mix clips; advance the transport; stop exactly at `endFrame` if stop-at-end is on.
7. Update meters (atomic fetch-max), publish transport atomics, record load and deadline misses, and push `RtEvent`s for the control thread.

**Verification.** `tests/engine/RealtimeSafetyTests.cpp` replaces global `operator new/delete` in the engine test binary and asserts **zero allocations and zero frees** over 200 callbacks that include a snapshot swap, queued seek/stop/play commands and RT events.

### 4.3 Communication primitives

| Primitive | Direction | Guarantees |
|-----------|-----------|------------|
| `SpscQueue<T>` | 1 producer → 1 consumer | Wait-free, bounded, preallocated |
| `SnapshotExchange<T>` | control → audio, with deferred delete back | `acquire()` is wait-free; objects are destroyed only on the control thread; superseded unseen objects are freed directly; a full retire queue is provably impossible in correct use, and if it happens anyway the stale object is leaked and counted rather than freed on the RT thread |
| `std::atomic` | audio → any | Transport position/state, meters, diagnostics counters |
| `SpscQueue<RtEvent>` | audio → control | Plain-data events (deadline miss, callback gap, rate mismatch, stop-at-end), aggregated before logging |
| Queued `QMetaObject::invokeMethod` | worker → Qt main | Results of media and analysis jobs; a generation counter drops results belonging to a previous project |

### 4.4 Lifetime rules

- The audio thread holds only raw pointers into the current snapshot. `PlaybackSnapshot::keepAlive` owns the assets, so no `shared_ptr` refcount traffic happens on the audio thread.
- The control thread must call `AudioEngine::collectGarbage()` periodically. `EngineBridge` does it at 30 Hz.
- Destruction order is device service → engine → JUCE runtime (`EngineBridge`), and project controller → analysis service → engine bridge (`MainWindow`). The controller waits for analysis callbacks (`waitIdle`) and joins its pool before dying.
- Device start/stop and engine control calls happen on the same (main) thread. While processing, commands are queued; while stopped, they apply immediately.

---

## 5. Audio engine

### 5.1 Transport

| Command | Effect |
|---------|--------|
| `Play` | `playing = true` |
| `Stop` | Halt at the current position. The UI's **Play/Pause** sends `Play`/`Stop`; its **Stop** button sends `Stop` + `Seek(0)` (stop and return to start); Home seeks to 0 without stopping; Play from the end of the content restarts at 0 |
| `Seek(frame)` | `playhead = max(0, frame)` |
| `SetStopAtEnd(bool)` | Default on. When the playhead reaches `snapshot.endFrame`, stop at exactly `endFrame` |

- Positions are `int64` frames at the engine rate. There is no floating-point accumulation; a simulated 24 h session at 192 kHz is exact (`TransportTests`).
- Commands are **block-accurate** in M1 (applied at the start of the next callback). Sample-offset event scheduling arrives with MIDI and automation in M3/M4.
- Output is **block-size invariant**: rendering with block sizes 1, 7, 64, 480, 513 and 4096 gives bit-identical output (regression test). This matters because WASAPI delivers variable block sizes.

### 5.2 Playback snapshot (M1 processing path)

`PlaybackSnapshot { version, sampleRate, tracks[{clips[{asset*, timelineStart, sourceStart, length, gain}], gainLeft, gainRight, key}], masterGain, endFrame, keepAlive }`

- Clips are sorted by `timelineStart`. Mono assets feed both channels. Outputs beyond 2 channels are left silent.
- **Track mixing is resolved on the control thread** (`trackChannelGains`): fader dB, pan, mute and solo become one linear gain per output channel, so the audio thread needs no mixer logic.
  - *Solo-in-place:* if any track is soloed, only soloed tracks are heard. Several tracks can be soloed. **Mute wins over solo.**
  - *Pan* is a balance law with **unity at centre**: panning right scales the left side by `cos(|pan|·π/2)` and leaves the right side alone; hard pan silences the far side exactly. A centred 0 dB track therefore renders bit-identically to the source. Mono media feeds both sides first. (A selectable pan law, e.g. −3 dB constant power for mono sources, is an M3 mixer item.)
- **Declicked gain changes.** When a snapshot changes a track's gains during playback, the engine ramps linearly to the new values over 10 ms (`rampFrames = round(0.01 × rate)`). Ramp state is matched across snapshots by `key` (FNV-1a hash of the `TrackId`) in two preallocated 256-entry arrays, so adoption stays allocation-free. Gains are computed as `start + step × n` from an integer frame count, which keeps ramps bit-identical for any block size. Changes made while stopped apply immediately. Fully silent tracks (gain 0, no ramp) are skipped.
- The **master fader** is folded into every track's gains on the control thread (`snapshot.masterGain` stays 1), so master moves are ramped exactly like track moves.
- Per-sample gain is `clip × track(channel) × master`.
- If the snapshot rate differs from the engine rate, the engine renders silence and reports `SampleRateMismatch` once per snapshot. This never happens in practice because the session layer only publishes engine-rate renditions.
- **Milestone 3** replaces the track list with a compiled processing graph (buses, sends, sidechains, plugins, latency compensation) published through the same `SnapshotExchange`. Graph compilation stays off the audio thread.

### 5.3 Diagnostics

| Counter | Definition |
|---------|------------|
| `lastLoad` / `peakLoad` | `process()` wall time ÷ real-time budget (`frames / rate`) |
| `deadlineMisses` | Blocks with load > 1.0 |
| `callbackGaps` | Interval between callbacks > 2 × block duration + 2 ms (likely dropout or driver stall) |
| `driverXruns` | `AudioIODevice::getXRunCount()`; −1 when the driver does not report it (WASAPI shared) |
| `droppedCommands` / `droppedEvents` | Full queues (should stay 0) |
| `snapshotSwaps` / `snapshotLeaks` | Adoptions / never-expected leaks |

### 5.4 Device service

- `AudioDeviceService` (JUCE `AudioDeviceManager`) has a JUCE-free API: `deviceTypes()`, `outputDevices(type)`, `describe(type, name)`, `open(setup)`, `close()`, `status()`, `currentSetup()`.
- **JUCE inside Qt:** `ScopedAudioRuntime` creates JUCE's `MessageManager` on the Qt main thread. JUCE posts its internal messages to a hidden window, and Qt's native Windows event loop dispatches them, so no second event loop is needed.
- Types: *Windows Audio* (WASAPI shared/exclusive/low-latency), *DirectSound*, and *ASIO* when `DAW_ENABLE_ASIO=ON` with `DAW_ASIO_SDK_DIR`. Check the ASIO SDK licence (Steinberg dual-licenses it) before distributing.
- Device errors are captured from `audioDeviceError` and surfaced in `status().lastError`. The saved device (QSettings) falls back to the system default if it fails.

### 5.5 Sample-rate policy

- WASAPI shared mode runs at the Windows mixer rate, and Suno material is commonly 44.1 or 48 kHz. Media is therefore never assumed to match the device.
- Each media file has an **original** (native rate, used for peaks and analysis) and a **rendition** at the engine rate, made by the offline resampler on a worker. When the device rate changes, renditions are re-targeted and the playhead is preserved in project time.

---

## 6. Media

### 6.1 Decoding

- `probeAudioFile` / `decodeAudioFile` support WAV, AIFF, FLAC, Ogg Vorbis and MP3 (JUCE readers). They are cancellable and report progress.
- **Memory-backed in M1**, bounded by `DecodeOptions` (default ≤ 8 channels, ≤ 30 min at 192 kHz, ≤ 2 GiB decoded float). Over-limit files fail with `LimitExceeded`. Disk streaming replaces this in M2 behind the same `AudioAssetPtr` consumer interface.
- Unicode paths are supported throughout (tests use non-ASCII directory and file names).

### 6.2 Resampler

Kaiser-windowed sinc (β = 8.6, 64 zero crossings per side), polyphase table (512 phases, linear interpolation), filter centred at 0.95 × the lower Nyquist:

- Pass band to ≈ 20 kHz, stop band from ≈ 21.8 kHz for 44.1 ↔ 48 kHz, ≈ −86 dB stop-band attenuation.
- Output length is `round(frames × ratio)`, time-aligned (no group-delay offset).
- Channels are processed in parallel. Tests check duration, amplitude, frequency, sample timing and alias rejection.

### 6.3 Peak pyramid

- Level 0: one min/max pair per 256 frames. Each further level merges 4 buckets, until ≤ 16 buckets remain.
- The view picks the coarsest level whose bucket ≤ frames-per-pixel, so drawing cost is independent of file length and zoom.
- A query is exact on bucket boundaries and conservative otherwise (never narrower than the true envelope).

### 6.4 Peak cache file (`Cache/waveforms/<mediaId>.peaks`)

Little-endian, version 1:

```
"IDPK" | u32 version | u64 sourceKey | f64 sampleRate | u32 channels | i64 frames
i64 baseSamplesPerBucket | u32 factor | i64 minBuckets | u32 levelCount
levelCount × { i64 samplesPerBucket, i64 numBuckets }
per level: numBuckets × channels × { f32 min, f32 max }   (channel-major)
u64 FNV-1a checksum of all preceding bytes
```

`sourceKey = FNV-1a(filename, size, last-write time)`. Caches are rejected and rebuilt when stale, corrupt, truncated or of an unknown version. They are written atomically and are always disposable.

---

## 7. Project persistence

### 7.1 Model and time base

- All timeline quantities (clip start, clip source offset, clip length, playhead) are `int64` frames at the **project sample rate**.
- The project rate is set by the first imported media (default 48 kHz). `TimeMapper` converts to engine frames (identity when the rates match).
- Every entity has a stable UUIDv4: project, track, clip, media, analysis record.

### 7.2 `project.json`, schema v1

```json
{
  "format": "stem-dawg-project",
  "schemaVersion": 1,
  "app": { "name": "Stem DAWg", "version": "0.1.0" },
  "project": { "id": "…uuid…", "name": "Rust Belt Liturgy",
               "createdUtc": "2026-11-21T21:30:00.123Z", "modifiedUtc": "…" },
  "timeline": { "sampleRate": 44100.0,
                "tempo": { "bpm": 120.0, "numerator": 4, "denominator": 4 } },
  "transport": { "playheadFrames": 66150 },
  "mixer": { "masterGainDb": 0.0 },
  "media": [
    { "id": "…", "name": "suno_drums", "path": "Media/Original/suno_drums.wav",
      "storage": "copied", "sampleRate": 44100.0, "channels": 2, "frames": 9261000,
      "sourceKey": "8f2c…16 hex digits…" }
  ],
  "tracks": [
    { "id": "…", "name": "suno_drums", "gainDb": 0.0, "pan": 0.0, "muted": false, "solo": false,
      "clips": [ { "id": "…", "name": "suno_drums", "mediaId": "…",
                   "timelineStart": 0, "sourceStart": 0, "length": 9261000, "gainDb": 0.0 } ] }
  ],
  "analyses": [
    { "id": "…", "mediaId": "…", "analyzer": "daw.transients", "analyzerVersion": 1,
      "result": "Analysis/<mediaId>/daw.transients-v1.json", "createdUtc": "…" }
  ]
}
```

- 64-bit keys are stored as hex strings, so JS and Python tooling never loses precision. Frame counts are JSON integers; nlohmann preserves `int64`.
- `pan` (−1…+1) and `solo` were added after the first v1 files. They are optional with defaults 0 and `false`, so older v1 files load unchanged and older builds ignore them, which needs no schema bump under the policy in 7.4. Out-of-range `pan` in a hand-edited file is clamped on load; the fader floor `gainDb = -144` means silence (shown as −inf).
- An optional per-track `"cleanup"` object (added in M2) stores the track's clean-up chain: `bypassed`, `dcRemoval`, `highPass {enabled, frequencyHz, slopeDbPerOctave}`, `expander {enabled, thresholdDb, ratio, rangeDb, releaseMs}`, since M4 `transient {enabled, attackDb, sustainDb}` and `resonance {enabled, harshDb, shimmerDb}`, and since M3 `hum {enabled, frequencyHz, harmonics}` and `denoise {enabled, reductionDb, sensitivityDb, profile {sampleRate, fftSize, seconds, source?, powerDb[]}}` (the noise profile at 0.01 dB resolution; `source` is `"selection"` when it was learned from a selected region). It is written only when it differs from the default (everything off). Missing or invalid values take their defaults, and numbers are clamped to their ranges, so neither older files nor hand edits can configure an unstable filter. No schema bump is needed (section 7.4). See [M2-Stem-Triage.md](../milestone-2/M2-Stem-Triage.md).
- An optional `"release": { "profileId": "…", "fields": { "title": "…", "album": "…", … } }` block stores the song-level export metadata and the artist profile chosen for the project (section 7.5). Only non-empty fields are written.
- Validation on load and save rejects non-positive rates, duplicate or missing ids, clips that reference unknown media, and negative positions or lengths.

### 7.3 Project directory

```
<Project>/project.json
Media/Original/  Media/Processed/  Media/Recorded/  Media/Samples/
Analysis/<mediaId>/<analyzer>-v<version>.json
Cache/waveforms/  Cache/spectrograms/
Presets/  PluginStates/  Backups/  Exports/
```

- **Copied** media (default) lives in `Media/Original` with a project-relative path, so the folder is portable; a test moves it to a Unicode-named path and reopens it. Name clashes become `name (2).wav`.
- **Referenced** media keeps an absolute path.
- **Processed** copies (voice denoise, added in M3; bleed suppression, M4) live in `Media/Processed`. Their media entry carries `"derivedFrom": {"source", "process", "amountDb"}` (`process` is `voice-denoise` or `bleed-suppress`), naming the media they were made from, which stays in the project; validation rejects a copy whose source is missing. Copies that neither the manifest nor the autosave uses are deleted when the project is closed; a clip whose copy is missing on open plays the original.

### 7.4 Save protocol, autosave and migrations

1. Validate → create the layout → copy the previous manifest to `Backups/project-<utc>.json` (keep the newest 10).
2. Write the new manifest **atomically**: temporary sibling → `FlushFileBuffers` → `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`.
3. Delete `Backups/autosave.json`.

Autosave writes `Backups/autosave.json` every 60 s while there are unsaved changes. On open, if the autosave is newer than the manifest, the user is offered recovery.

**Migrations** run on the raw JSON before mapping onto the model, one step per version (`N → N+1`), via `MigrationRegistry`. Policy:

- Bump `schemaVersion` for any change an older build cannot read, and ship a step plus a fixture test with it.
- Files newer than the running build are refused (`VersionTooNew`), never silently truncated.

The tests drive a hypothetical v0 → v1 migration (seconds → frames) end to end.

### 7.5 Artist profiles and export metadata

- **Profiles** live in `%LOCALAPPDATA%\StemDAWg\profiles.json` (`"format": "stem-dawg-profiles"`, schema 1), outside projects, so artist details are entered once. Each profile has an id, a display name and metadata fields (artist, album artist, composer, label, copyright, genre, comment, website, artwork path). The library remembers the active profile, which new projects adopt.
- **Resolution at export:** song fields from the project override profile fields; an empty title falls back to the project name and an empty year to the current year; `{year} {artist} {title} {album}` placeholders are expanded in the copyright and comment.
- **Written tags:** a RIFF `LIST/INFO` chunk before the audio (INAM, IART, IPRD, ITRK, ICRD, IGNR, ICOP, ICMT, ISFT; UTF-8) and an `id3 ` chunk after it carrying **ID3v2.3** (TIT2, TPE1, TPE2, TALB, TRCK, TYER, TCON, TCOM, TPUB, TCOP, TSRC, TSSE, COMM, WOAR, APIC front cover). v2.3 rather than v2.4 for compatibility; ASCII text is ISO-8859-1, anything else UTF-16 with BOM. TCOP drops the leading © as the ID3 spec requires. A reader (`readWavTags`) supports v2.3/v2.4 and is used by the tests.

### 7.6 Final render (Export Mix)

- `renderMix` runs a private `AudioEngine` over the same snapshot type the live engine plays, so exports match playback (mixer, pan law, mute/solo, ramps). It renders **at the project sample rate from the original media**, resampling only media whose native rate differs, never from the device-rate playback copies.
- Formats: WAV 24-bit PCM (default), 16-bit PCM with TPDF dither (±1 LSB, deterministic seed), 32-bit float (keeps overs). Files are written atomically. A file over 4 GB is written as RF64 (EBU Tech 3306, added in M2): a `ds64` chunk carries the 64-bit sizes, and the tag reader understands it.
- The export report gives size, duration, sample peak and the number of clipped samples; clipping shows a warning with the master reduction needed. The export dialog warns when solo or mute will leave tracks out.

---

## 8. AI subsystem (integrated from day one)

### 8.1 Service

`AnalysisService` is a priority queue (higher priority first, FIFO within a priority) with a worker pool at below-normal OS priority.

- **Job lifecycle:** `Queued → Running → Succeeded | Failed | Cancelled`.
- Progress is reported in [0, 1]. Cancellation is cooperative: queued jobs are cancelled immediately, and running jobs stop at their next check (analyzers poll at least every ~50 ms of work).
- Completion callbacks run on the worker before the state turns terminal, so `wait()` and `waitIdle()` return only after callbacks have finished. Owners can therefore shut down deterministically.

### 8.2 Results are data

Every result is JSON stored at `Analysis/<mediaId>/<analyzer>-v<version>.json` and referenced from `project.json`. Nothing modifies audio.

In M1, transient candidates become timeline markers that are restored when the project reopens. In M5 they become editable kick and snare trigger events.

### 8.3 Built-in analyzers

| Id | Output | Purpose later |
|----|--------|---------------|
| `daw.levels` | Per-channel peak, RMS, crest, DC offset, clipped samples; stereo correlation | Clip and level diagnostics, mastering pre-checks |
| `daw.transients` | Onset candidates `{frame, timeSec, strength}`: spectral flux (1024/256, log-compressed), adaptive median threshold, offset rejection, attack refinement on a high-passed envelope | Baseline and reference for the M6 learned kick/snare onset model; trigger lanes in M5 |
| `daw.spectrum` | 8-band energy distribution, centroid, harshness ratio (2–5 kHz share) | First input to Suno guitar harshness detection |
| `daw.inference-probe` | Runs audio through the ONNX path; reports provider, latency, error | Proves the model path end to end |

The transient detector is regression-tested on a Suno-style fixture: kicks under a sustained 55/82.5 Hz drone, decoded from 16-bit WAV. All 24 hits must be found within 3 ms. An earlier version placed hits 15–25 ms early when a drone was present and produced a phantom hit where a stem ended on a non-zero sample. Both cases are now covered by tests.

### 8.4 Inference backend

- `InferenceBackend` is the interface; `createOnnxBackend()` is the ONNX Runtime implementation. Sessions are shared, `Run` is thread-safe, and ORT headers never leave `ai/src`.
- **Execution providers:** the preferred list per model is tried in order (`directml`, `cuda`, `cpu`), only if the provider is present in the loaded ORT build, falling back to CPU with a log entry.
- Intra-op threads default to 2 so inference cannot starve the audio thread. Lowering ORT's thread priority through custom thread hooks is planned for M6.
- **Model registry:** `models.json` manifest (`id`, `version`, `task`, `file`, `providers`). Relative paths resolve against the manifest. Entries are validated at registration and models load lazily.
- **Built-in self-test model** (`daw.selftest.scale2`, y = 2x) is generated in code as ONNX protobuf (IR 8, opset 13). It runs at app start (diagnostics panel shows backend, provider and latency) and in tests, so the AI path is exercised without shipping model files.

---

## 9. User interface (M1)

- **Track headers** (`TrackPanel`, left of the lanes, Suno Studio–style): colour strip, number and name, **M**ute and **S**olo buttons, a volume fader (fader taper −inf…+6 dB, 0 dB detent, double-click resets) with a dB readout, and a pan knob (drag up/right, Shift for fine, wheel steps, double-click centres) with an L/C/R readout. Headers stay aligned with the lanes and scroll with them. Muted and solo-silenced tracks are dimmed in both the header and the lane. Edits go straight to `ProjectController` → model → new snapshot (cheap: pointers only) and mark the project dirty.
- **Arrangement view** (`WaveformView`): coloured lanes (colour from the stem name: vocals, drums, bass, guitar, keys, synth…), a time ruler, clip waveforms from peak pyramids with stereo channels split or combined (View → Split Stereo Channels), analysis markers, playhead with page-follow, click to seek (drag on the ruler to scrub), a time selection dragged across the lanes (Shift+click extends, Esc clears; added in M3), Ctrl+wheel to zoom around the cursor, wheel to scroll lanes vertically (horizontally when all lanes fit, or with Shift), zoom-to-fit.
- **Transport bar:** Return to start (Home), Play/Pause (Space), Stop (stop and return to start), position as `mm:ss.mmm` plus `bar.beat.tick` (960 PPQ), Analyze, the **artist profile switcher**, and the **master section** (fader, dB readout, CLIP light that latches when the output exceeds 0 dBFS; click to reset).
- **Hover help:** every control, menu item, diagnostics row and dialog field has a tooltip with a bold title and a plain-language explanation (including which tag each metadata field writes). The dark theme uses white typed text and light-grey placeholder hints.
- **Settings → Artist Profiles** (`ProfileDialog`) and **File → Export Mix** (Ctrl+E, `ExportDialog`): empty song fields show the inherited profile value as placeholder text, and a summary shows the resolved artist, label, copyright, website and artwork.
- **Import order:** a multi-file import is sorted naturally by file name ("2 Drums" before "10 FX"), and tracks are added in that order even though files decode in parallel.
- **Docks:** Diagnostics (device, format and latency, DSP load, dropouts, engine counters, AI status, output meters, live log) and Analysis (result summaries).
- **File menu:** New, Open, Save, Save As, Import Audio (Ctrl+I), drag-and-drop import; command-line paths open a project folder or import files.
- **Audio menu:** device dialog (type, device, rate, buffer), persisted in QSettings.
- UI refresh is 30 Hz from atomics. The UI never blocks on the engine, and the engine never calls into Qt.

The other five workspaces (Mixer, Piano Roll, Spectral, Reconstruction, Mastering) arrive with their milestones.

---

## 10. Diagnostics and logging

- JSON lines at `%LOCALAPPDATA%\StemDAWg\logs\daw-YYYYMMDD.jsonl`:
  `{"ts","level","cat","msg","tid","fields":{…}}`
- Categories: `app`, `audio`, `engine`, `media`, `project`, `ai`, `ui`.
- The audio thread never logs. `RtEvent`s are drained at 30 Hz, and timing problems are aggregated to at most one warning per second.
- Sinks run on the writer thread. `removeSink` guarantees the sink is not running when it returns, so the Qt panel can detach safely.

---

## 11. Testing strategy

Run with `ctest --preset local-debug` (or `-C Release`). There are 213 CTest cases in 7 executables (as of M4), all generated audio (no binary fixtures):

| Binary | Covers |
|--------|--------|
| `daw_core_tests` | SPSC FIFO, wrap-around and 2-thread stress; snapshot exchange (adopt, retire, superseded, 20 000-publish contention, no leaks); UUIDs; Result; cancellation; FNV vectors; atomic writes incl. Unicode paths; timestamps; logger delivery and audio-thread refusal |
| `daw_dsp_tests` | FFT; BS.1770-4 K-weighting coefficients; EBU Tech 3341 tests 1-5 and Tech 3342 loudness range; true peak; mono/stereo weighting; block-size invariance; allocation-free metering; clean-up chain response, expander timing and block-size invariance (added in M2, see [M2-Stem-Triage.md](../milestone-2/M2-Stem-Triage.md)); spectral noise reduction, hum notches, listen-to-removed (added in M3, see [M3-Noise-Removal.md](../milestone-3/M3-Noise-Removal.md)); bleed suppression, the transient shaper and harshness and shimmer control (added in M4, see [M4-Artifact-Repair.md](../milestone-4/M4-Artifact-Repair.md)) |
| `daw_engine_tests` | Transport semantics and 24 h exactness; sample-exact clip rendering; **block-size invariance**; seek; stop-at-end exact frame; mono→stereo and gain; rate mismatch; snapshot swap mid-play; queued vs immediate commands; meters; per-channel track gains; **10 ms gain ramps** (monotonic, bounded step, exact after, block-size invariant), no ramp while stopped or for new tracks; **zero heap use in `process()`** incl. ramp re-matching over 16 reordered tracks; concurrent control/audio threads |
| `daw_media_tests` | WAV metadata round trip through RIFF INFO + ID3v2.3 (ASCII and Unicode, artwork, padding, RIFF size); tagged files decode to identical audio; deterministic, unbiased ≤1 LSB TPDF dither; WAV round trip (16/24/float × mono/stereo); missing, unsupported, over-limit; cancellation; Unicode paths; peak pyramid vs brute force (all levels); query exactness/conservativeness; level selection; cache round trip; stale, corrupt, truncated and missing caches; source keys; resampler quality and timing (44.1↔48, 48→96), alias rejection, cancellation |
| `daw_project_tests` | Lossless round trip of a project using every field (incl. >32-bit frame counts, Unicode); format/version errors; validation (incl. pan range); defaults (incl. legacy files without pan/solo); metadata precedence and placeholders; profile library round trip, repair and version refusal; release block; migration chain, missing/failing/throwing steps; layout; backups rotation; refusal of invalid models; autosave recovery rules; portable copied media; referenced media |
| `daw_ai_tests` | Level analyzer on known signals; transients on clicks, **drone-masked kicks**, silence, steady tone; spectral bands; cancellation; service results, callbacks, cancellation, failures, priorities, shutdown; model registry/manifest; ONNX init, self-test, file models with Unicode paths, 4-thread concurrent inference, provider fallback; probe analyzer in a job |
| `daw_integration_tests` | Import/staging; project-rate rules; media loading with peak cache hit/rebuild and renditions; snapshot time mapping and missing media; fader/pan/mute/solo-in-place resolution; stable per-track snapshot keys; render equals the expected mix incl. pan, mute and master; clipping report; project-rate render with resampled media; tagged export matches the render bit-for-bit; decode→analyze regression; **definition of done** (bit-identical reopen at native and resampled rates); background analysis without deadline misses |

Regression measures planned for M3 onward (in the roadmap): rendered-output hashes per fixture project, phase and timing offsets after latency compensation, LUFS and true-peak deltas.

---

## 12. Dependencies and licensing

**Stem DAWg is licensed AGPL-3.0-or-later** (Copyright © 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors). Every first-party file carries an SPDX header; the About box shows the AGPL notices required for interactive programs (section 5(d)).

| Dependency | Version | Licence | Compatibility / obligations |
|------------|---------|---------|-----------------------------|
| Qt | 6.5+ (dev 6.12.0) | LGPL-3.0 | Dynamically linked, DLLs replaceable; ship licence text |
| JUCE | 8.0.7 | **AGPL-3.0** (open-source option) | Same licence as the project |
| ONNX Runtime | 1.22.0 | MIT | Notice |
| nlohmann/json | 3.11.3 | MIT | Notice |
| TinySoundFont / TinyMidiLoader | commit a3621a1 | MIT | Notice (added in M2: MIDI playback) |
| XZ Embedded | 2024-12-30 | 0BSD | None required (added in M2: SoundFont download) |
| Catch2 | 3.7.1 | BSL-1.0 | Test-only |
| FluidR3_GM SoundFont | Debian 3.1-5.3 | MIT | Downloaded at run time on request, never shipped; credit Frank Wen |
| ASIO SDK | optional | GPL-3.0 option (since Oct 2025) or proprietary | GPL-3.0 is AGPL-compatible (section 13); off by default |
| VST 3 SDK | M3+ | MIT (since 3.8, Oct 2025) | Notice |
| Models, IRs, drum samples | M3+ | Per asset | AGPL-compatible licence and training-data provenance required |

Details and rules for new dependencies: `THIRD_PARTY_NOTICES.md`.

---

## 13. Known limitations (deliberately deferred)

| Limitation | Planned |
|------------|---------|
| Whole-file in-memory media | M2: disk streaming with prefetch |
| Block-accurate transport commands; no click-free fades on stop/seek (mixer gain changes are already ramped) | M2: declick ramps; M3/M4: sample-offset event scheduling |
| Single read-only clip per import, no editing or undo | M2: clip editing, command-based undo/redo |
| No automatic reopen after device loss | M8 hardening (state and error are surfaced now) |
| Waveform drawn with QPainter | Move to QRhi when lane counts make it necessary |
| ORT CPU package by default | DirectML package for GPU inference on Windows (M6) |
| ASIO untested on hardware in CI | Manual validation matrix in M8 |

---

## 14. Risks

| Risk | Mitigation in M1 |
|------|------------------|
| RT regressions as features land | Allocation-guard test, block-size invariance test, `noexcept` RT path, logger refusal on the audio thread |
| JUCE/Qt event-loop interplay | Single runtime object on the main thread; JUCE messages dispatched by Qt's native loop; device lifecycle confined to the main thread |
| Project format drift | Schema version, migration registry with tests, refusal of newer files, atomic writes, backups, autosave |
| AI work starving audio | Separate low-priority workers, capped ORT threads, no inference API reachable from `engine` |
| Suno bleed fooling detection | Results are editable data; a drone-masked fixture guards the transient baseline |
