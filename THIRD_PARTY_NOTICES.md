# Third-party components

Stem DAWg is licensed under the **GNU Affero General Public License v3.0 or later** (see [LICENSE](LICENSE)). It is built with the components below. Every licence listed is compatible with distributing the combined program under AGPL-3.0-or-later.

| Component | Version | Licence | Used for | Notes |
|-----------|---------|---------|----------|-------|
| [JUCE](https://juce.com) | 8.0.7 | AGPL-3.0 (open-source option of its dual licence) | Audio devices, audio file decoding | Only non-GUI modules, compiled into a static library. JUCE's AGPL option is why Stem DAWg is AGPL. |
| [Qt](https://www.qt.io) | 6.5+ (developed with 6.12) | LGPL-3.0 | Desktop user interface | Linked dynamically. The Qt DLLs ship unmodified beside the executable and can be replaced by the user. Qt source: https://download.qt.io |
| [ONNX Runtime](https://onnxruntime.ai) | 1.22.0 | MIT | AI model inference | Downloaded from the official NuGet package at build time. |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | Project, profile and analysis files | |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) | commit a3621a1 | MIT | SoundFont synthesis | Single-header library, compiled into the media module. |
| [Basic Pitch](https://github.com/spotify/basic-pitch) model (Spotify) | ICASSP 2022 model, commit fa5997a | Apache-2.0 | Notes to MIDI (pitched stems) | The ONNX export `nmp.onnx` (230 KB), unmodified, in `third_party/basic-pitch` with its licence, compiled into the ai module. "Copyright 2022 Spotify AB." Bittner et al., "A Lightweight Instrument-Agnostic Model for Polyphonic Note Transcription and Multipitch Estimation", ICASSP 2022. Its note decoding is reimplemented in C++ (`ai/NoteTranscriber`). |
| [XZ Embedded](https://github.com/tukaani-project/xz-embedded) | 2024-12-30 | 0BSD | Unpacking the SoundFont download (.xz) | Decoder only. |
| [Catch2](https://github.com/catchorg/Catch2) | 3.7.1 | BSL-1.0 | Unit tests | Test-only, not shipped. |
| Microsoft Visual C++ runtime | 14.44 | Microsoft Visual Studio redistributable ("Distributable Code") | C++ standard library at run time | Binary releases only: the unmodified redistributable DLLs (`vcruntime140*.dll`, `msvcp140*.dll`, `concrt140.dll`) ship beside the executable so no separate installer is needed. They are system libraries in the sense of AGPL section 1. |

## Downloaded at run time (not part of the source or the build)

| Component | Licence | Notes |
|-----------|---------|-------|
| [FluidR3_GM SoundFont](https://packages.debian.org/source/sid/fluid-soundfont) by Frank Wen | MIT | Downloaded only when the user asks (Settings > SoundFont for MIDI), from Debian's `fluid-soundfont-gm` 3.1-5.3 package, and verified with SHA-256. "Copyright (c) 2000-2002, 2008 Frank Wen. I hereby release Fluid under the MIT license." |
| [DeepFilterNet3](https://github.com/Rikorose/DeepFilterNet) voice model by Hendrik Schröter | MIT or Apache-2.0 | Downloaded only when the user asks (Clean-up > Voice denoise), as the ONNX export `models/DeepFilterNet3_onnx.tar.gz` pinned to commit 84d57ec of the project's repository, and verified with SHA-256 (the archive and each graph). "Copyright (c) 2021 Hendrik Schröter." Stem DAWg reimplements its feature extraction and output stages in C++ (`dsp/DeepFilterDsp`), following the reference implementation (libDF, same licence). |

## Optional and planned components

| Component | Licence | Status |
|-----------|---------|--------|
| [Steinberg ASIO SDK](https://www.steinberg.net/developers/) | GPL-3.0 (open-source option since October 2025) or proprietary | Optional (`DAW_ENABLE_ASIO`, off by default). GPL-3.0 is compatible with AGPL-3.0 (section 13 of both licences). Builds that enable it must use the SDK under GPL-3.0. "ASIO" is a Steinberg trademark. |
| [Steinberg VST 3 SDK](https://github.com/steinbergmedia/vst3sdk) | MIT (since VST 3.8, October 2025) | Planned for plugin hosting. "VST" is a Steinberg trademark. |
| Further AI models | Must be MIT, Apache-2.0, BSD or similarly permissive, or GPL/AGPL-compatible | Each model's licence and the provenance of its training data are recorded before it is offered. |

## Rules for new dependencies

1. The licence must be compatible with AGPL-3.0-or-later: permissive (MIT, BSD, Apache-2.0, BSL-1.0, zlib), LGPL with dynamic linking, or GPL-3.0/AGPL-3.0. GPL-2.0-only code cannot be used.
2. Add the component to this file in the same pull request.
3. No proprietary SDKs in default builds.
4. No copyrighted audio in the repository: test audio is generated in code.

The full licence texts of bundled components are included with binary distributions under `licenses/`.
