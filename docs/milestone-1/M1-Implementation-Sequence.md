# Stem DAWg — Milestone 1 Implementation Sequence

Ordered so that each step ends with something testable and later steps never force rework of earlier ones. Real-time and persistence foundations come first; the UI comes last because it only consumes the layers below.

Status key: **Done** = implemented and covered by passing tests in this repository.

| Step | Deliverable | Key files | Exit criteria (tests) | Status |
|------|-------------|-----------|-----------------------|--------|
| 1 | **Build skeleton.** CMake ≥ 3.25, presets, compile options (`/W4 /permissive- /utf-8`), dependency fetch (JUCE, json, Catch2, ONNX Runtime), single-compile JUCE static lib, module helper | `CMakeLists.txt`, `cmake/*.cmake`, `third_party/juce_static/` | Configure and build with 0 warnings; empty test binaries run | Done |
| 2 | **Core vocabulary.** `Result`/`Status`, typed UUID ids, FNV hash, UTF-8 path helpers, atomic file writes, timestamps, `AudioAsset` | `core/` | `CoreUtilityTests` | Done |
| 3 | **RT primitives.** `SpscQueue`, `SnapshotExchange` (deferred delete), `RtEvent`, `ScopedFlushDenormals`, audio-thread marker | `core/include/daw/core/*` | `SpscQueueTests`, `SnapshotExchangeTests` (incl. contention) | Done |
| 4 | **Structured logging.** JSON lines, writer thread, sinks, audio-thread refusal | `core/src/Logger.cpp` | Logger test | Done |
| 5 | **Transport + engine core.** Integer transport, command queue, snapshot adoption, clip mixing, stop-at-end, meters, diagnostics, RT events | `engine/` (`Transport.h`, `AudioEngine.*`, `PlaybackSnapshot.h`) | `TransportTests`, `AudioEngineTests` (sample-exact, block-size invariant) | Done |
| 6 | **RT verification harness.** Global allocation guard, zero-allocation test, concurrent control/audio test | `tests/support/AllocationGuard.*`, `RealtimeSafetyTests` | 0 allocations / 0 frees across swaps and commands | Done |
| 7 | **Device layer.** JUCE-free `AudioDeviceService` API, JUCE `AudioDeviceManager` implementation, `ScopedAudioRuntime` (JUCE MessageManager under Qt) | `engine/src/JuceAudioDeviceService.cpp`, `AudioRuntime.cpp` | Manual: device opens (log `audio/device opened`), DSP load and counters update | Done (manual) |
| 8 | **Media decode + WAV writer.** JUCE readers behind a pure API; limits, cancellation, progress; dependency-free WAV writer | `media/AudioFileDecoder.*`, `WavWriter.*` | `DecoderTests` | Done |
| 9 | **Peaks.** Pyramid build/query, binary cache with checksum and source key | `media/PeakPyramid.*`, `PeakCache.*` | `PeakPyramidTests`, `PeakCacheTests` | Done |
| 10 | **Resampler.** Kaiser-sinc polyphase SRC for engine-rate renditions | `media/Resampler.*` | `ResamplerTests` (timing, amplitude, aliasing) | Done |
| 11 | **Project model + JSON schema v1.** Model, serializer, validation | `project/ProjectModel.*`, `ProjectSerializer.*` | `SerializationTests` | Done |
| 12 | **Migrations.** Registry, version policy, refusal of newer files | `project/Migrations.*` | `MigrationTests` | Done |
| 13 | **Project store.** Directory layout, atomic save, backup rotation, autosave/recovery, media staging (copy/reference), path resolution | `project/ProjectStore.*` | `ProjectStoreTests` (incl. moved Unicode project folder) | Done |
| 14 | **Session glue.** Media pool, loader (decode → peaks/cache → rendition), import operation, project↔engine time mapping, snapshot builder | `session/` | `SessionTests` | Done |
| 15 | **AI service.** Job queue, priorities, worker pool at low priority, progress, cancellation, callbacks, shutdown semantics | `ai/AnalysisService.*` | `AnalysisServiceTests` | Done |
| 16 | **Inference backend.** `InferenceBackend` interface, ONNX Runtime implementation, provider selection and fallback, model registry and manifest, in-code self-test model | `ai/OnnxBackend.cpp`, `ModelRegistry.*`, `BuiltinModels.*` | `InferenceTests` (incl. 4-thread concurrency) | Done |
| 17 | **Deterministic analyzers.** Levels, transients, spectrum, inference probe; results stored as JSON project data | `ai/src/*Analyzer.cpp`, `Fft.*` | `AnalyzerTests`, drone-masked transient regression | Done |
| 18 | **Definition-of-done integration.** Headless load → peaks → play → stop → seek → save → reopen → bit-identical render; analysis concurrent with rendering | `tests/integration/DefinitionOfDoneTests.cpp` | Both rates pass; 0 deadline misses | Done |
| 19 | **Qt shell.** `EngineBridge` (30 Hz poll, RT event aggregation), `ProjectController` (worker pool, generation guard, autosave, analysis persistence, markers), `WaveformView`, `DeviceDialog`, `DiagnosticsPanel`, menus, shortcuts, drag-and-drop, command-line paths, dark theme | `apps/desktop/` | Manual: launch, device opens, ONNX self-test passes, WAV imports, waveform and markers display, analysis runs | Done (manual) |
| 20 | **Release build + docs.** Release configuration passes all tests; spec, sequence and README | `docs/`, `README.md` | 90/90 tests in Debug and Release | Done |
| 21 | **Track headers and transport polish** (from first user review). Mute/solo/fader/pan headers aligned with lanes and scrolling with them; control-thread gain resolution (solo-in-place, mute wins, unity-centre balance pan); 10 ms declick ramps in the engine; Play/Pause + Stop-returns-to-start; ordered parallel import with natural sort; stem colours; split/combined channel view | `apps/desktop/TrackPanel.*`, `TrackStyle.h`, `engine/AudioEngine.*`, `session/SnapshotBuilder.*`, `project/` | Mixer and ramp tests, RT allocation test with ramps (99/99 Debug and Release); manual: solo/mute dimming, fader and pan readouts, transport icons | Done |

| 22 | **Hover help, master, artist profiles and Export Mix** (second user review). Tooltips everywhere; master fader + clip light (master folded into ramped track gains); profile library and project release metadata; offline render at project rate; WAV export with RIFF INFO + ID3v2.3 + artwork and TPDF dither; ordered export report with clipping advice | `media/AudioTags.*`, `media/WavWriter.*`, `project/Metadata.*`, `session/Renderer.*`, `apps/desktop/ProfileDialog.*`, `ExportDialog.*`, `HelpText.h` | `AudioTagsTests`, `MetadataTests`, `RenderTests` (112/112); manual: profile switcher, export dialog, tagged file verified with an independent parser | Done |

| 23 | **Open-source release prep: Stem DAWg.** Rebrand (app, exe, data folder, file formats with legacy aliases, settings migration); AGPL-3.0-or-later with SPDX headers; THIRD_PARTY_NOTICES, CONTRIBUTING, CI; multi-size icon (16-256 px, head crop for small sizes), version resource, AppUserModelID, About box with AGPL notices, logo in README and empty arrangement | `LICENSE`, `apps/desktop/resources/`, `assets/branding/`, `.github/workflows/ci.yml` | 113/113 tests incl. legacy-format regression; manual: icon in title bar and taskbar | Done |

The direction after Milestone 1 (cleaning, noise removal, mastering, audio-to-MIDI) is in [../ROADMAP.md](../ROADMAP.md).

## Remaining Milestone 1 hardening (before starting Milestone 2)

These are small, independent tasks, listed in suggested order.

1. **CI.** A GitHub Actions (or local `ueq`-style) job: configure with the `vs2022-headless` preset, build Debug and Release, run `ctest`. Cache `build/_deps`.
2. **Device-loss handling.** On `audioDeviceError` or device removal, mark the engine stopped, show a banner, and offer a one-click reopen of the saved or default device.
3. **Declick on stop/seek.** A 5 ms fade-out/fade-in inside `process()` (needs one preallocated scratch buffer). Add a test that the first and last samples around a seek are ramped. Mixer gain changes are already ramped (step 21); stop/seek can reuse the same ramp shape.
4. **Hardware validation matrix.** WASAPI shared and exclusive, DirectSound, ASIO (if licensed) at 44.1, 48 and 96 kHz with 64–1024 sample buffers; record DSP load and dropouts per configuration.
5. **Large-file soak.** A 30-minute stereo 96 kHz import: memory budget message, resample time, peak build time, UI responsiveness.

## Milestone 2 entry points prepared by this design

- **Undo/redo:** `ProjectModel` is a value type and every mutation already goes through `ProjectController` or `session/ProjectOperations`. Convert these into `Command` objects with `apply`/`revert`.
- **Disk streaming:** the engine consumes `const AudioAsset*` through snapshots only. Introduce a streaming source with a prefetch ring behind the clip interface without changing transport or exchange code.
- **Clip editing:** clips already carry `timelineStart`, `sourceStart` and `length` in project frames, and snapshots rebuild on every model change.
- **Processing graph (M3):** replace `TrackPlayback` with a compiled graph published through the same `SnapshotExchange`.
