<p align="center">
  <img src="docs/images/stem-dawg-logo.png" alt="Stem DAWg" width="560">
</p>

<p align="center">
  <b>Clean up AI-generated stems and home recordings. Balance them. Master them.</b><br>
  Built for industrial, EBM and techno first.
</p>

<p align="center">
  <a href="https://github.com/Deverydoo/Stem-DAWg/actions/workflows/ci.yml"><img src="https://github.com/Deverydoo/Stem-DAWg/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0--or--later-blue" alt="License: AGPL-3.0-or-later"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-informational" alt="Windows">
</p>

---

**Stem DAWg** is a music cleaner and mastering tool. It is not another general-purpose DAW. You load the stems from Suno (or any stem export) or your own recordings, and it helps you remove what ruins them: separation bleed, noise beds, fan hiss, harsh and phasey high ends, stems that sum into clipping. Then you balance and master the result.

> **Status: early development (Milestones 1–5 done).** It plays, mixes, analyses, cleans up, removes noise and hum (including an AI voice denoiser for speech), suppresses bleed between stems, sharpens soft AI drums, tames harsh resonances and generator whine, repairs the stereo image, masters to a loudness target through a true-peak limiter and a soft clipper, compares the tonal balance with reference tracks you choose, exports the finished mix, mastered for streaming, Bandcamp, club or CD, and turns stems into MIDI: drums, bass, guitars, keys, synths and voices. Next on the [roadmap](docs/ROADMAP.md): beats for songs without drums.

## What works today

- **Stem report:** right after import, every stem is measured (loudness, share of the mix, noise bed, activity, low end, stereo, how fast its hits rise, resonances that ring, how mono the low end is, what a mono sum loses) and compared with the others (shared low end, bleed). Plain-language advice follows, for example *"Bass and Synth play the same low end: keep it in Bass and high-pass Synth around 120 Hz"* or *"Drums has a noise bed at -53 dBFS: an expander near -47 dB will clean the gaps"*.
- **Noise removal:** learned-profile spectral noise reduction for fans, air conditioning and hiss (one click learns the noise from the track's quiet moments), automatic mains-hum detection and removal, an AI **voice denoiser** for spoken recordings (DeepFilterNet3, an optional 8 MB download, running locally), and **Listen to Removed** to hear exactly what is taken out.
- **Bleed suppression:** every stem is judged against all the others and turned down where another stem owns the sound: the hi-hat ghosting through the vocal, the kick inside the bass. On a Suno export the beds between notes drop by 11–19 dB while the music stays within 0.1–1.6 dB. It renders copies; the originals stay in the project.
- **Transient restoration:** AI-generated drums take 2–4 ms to rise to each hit, where a sampled drum takes about 1 ms, so they sound soft. The stem report measures it per stem, and a transient shaper in the clean-up chain makes the hits rise faster without making them louder (on a Suno drum stem, 2.5 ms before each peak the hits are 3 dB further along). Sustain tightens or lengthens the decay.
- **Harshness and shimmer control:** AI songs carry narrow peaks that keep ringing above the sound around them: a partial at 2–4 kHz that outlasts the notes, or a steady tone near 16 kHz that the generator leaves behind (heard as a whine or a metallic shimmer). The stem report finds them, and a resonance suppressor turns them down while they ring, leaving melodies and the rest of the high end alone.
- **Stereo repair:** separated stems often carry a wide or out-of-phase low end, and some AI songs carry the same sound in both channels out of step, so it partly cancels on phones and club systems. Mono bass makes everything below 120 Hz mono; phase alignment brings the channels into step without changing their levels (up to 1.7 dB back in a mono sum on the Suno songs measured); width narrows or widens by hand.
- **Clean-up chain per stem:** DC removal, high-pass, stereo repair, hum removal, noise reduction, harshness and shimmer control, a noise-bed expander and a transient shaper on every track, live and non-destructive. The stem report suggests the settings (*"Fix: Bass: expander at -31 dB"*) and **Apply All Fixes** sets them up. The expander looks 5 ms ahead, so note attacks stay intact.
- **Loudness metering (EBU R 128 / ITU-R BS.1770-4):** live momentary, short-term and integrated loudness, loudness range and true peak in the transport bar, in the stem report and in the export summary. Verified against the EBU Tech 3341/3342 test cases.
- **MIDI playback:** import `.mid` files (such as the MIDI from Suno's advanced stem split) as tracks next to the audio. They play through a General MIDI SoundFont, by default FluidR3_GM, downloaded and verified in the app, and follow the song's tempo map. The first MIDI file sets the project tempo.
- **Stem mixer, Suno Studio–style:** a coloured lane per stem with mute, solo, volume and pan. Every mixer change is declicked.
- **Real-time engine:** zero allocations in the audio callback (enforced by tests), sample-exact playback, WASAPI / DirectSound (ASIO optional).
- **Projects:** versioned `project.json`, atomic saves, backups, autosave and recovery, stems copied into the project folder.
- **Master bus and loudness targets:** EQ, a three-band compressor, a 4x-oversampled soft clipper and a true-peak limiter after the summed tracks keep the master under its ceiling (−1 dBTP by default), inter-sample peaks included. **Match Loudness** sets the master fader so the master reaches a target (streaming −14, Apple Music −16, CD/Bandcamp −10, club −8 LUFS) through the limiter, switches on the soft clipper when the limiter alone cannot get there (it rounds off the loudest peaks, which keeps more of the song's dynamics), and says plainly when a target would take more than the mix can stand.
- **Tonal balance against your references:** point it at a folder of releases you want to sound like; it measures them, compares the mix per third of an octave, and suggests (and applies) master EQ moves. No genre curves are invented: the target is your references.
- **Export Mix:** renders the whole song at the project's sample rate to WAV (24-bit, 16-bit dithered, 32-bit float), master bus included, with clipping detection. Or **mastered for a destination**: streaming (−14 LUFS), Bandcamp (−10), club (−8) or CD (16-bit 44.1 kHz), or all four at once, one file each, each matched to its loudness and ceiling at its own sample rate without touching your project.
- **Drums to MIDI:** right-click a drum track (or Analysis → Drums to MIDI) for a General MIDI file of its kick, snare, hi-hats, crash and toms, on the song's beats and quantised, with a tempo change on every beat so it stays on the song even as an AI song's tempo drifts. It is saved and added as a MIDI track to hear against the stem; rebuild the beat with your own drum samples.
- **Bars that follow the song:** Analysis → Track the Beats finds the song's beats on its drum track, so the timeline's bars and the position follow an AI song's drifting tempo, selections snap to its beats and bars, and the MIDI files share them.
- **Notes to MIDI:** right-click a bass, guitar, keys, synth or vocal track for a MIDI file of its notes, transcribed by Spotify's Basic Pitch model (runs on your computer, built in). With a drum track in the project the file follows the song's beats like the drum file, so the two line up in any DAW; the instrument is chosen from the track's name.
- **Artist profiles:** enter your artist details once (artist, label, copyright, genre, website, cover art) and every export gets RIFF INFO and ID3v2.3 tags plus embedded artwork. Switch between artists in the toolbar.
- **AI built in:** ONNX Runtime inference and background analyzers for levels, transients, spectrum and an inference probe. These are the foundation for AI-assisted cleaning and tuning.
- **Hover help** on every control.

## Build

Requirements: Windows 10/11, Visual Studio 2022 (Desktop development with C++), CMake ≥ 3.25, Qt 6.5+ for MSVC 2022 x64. JUCE, ONNX Runtime, nlohmann/json and Catch2 are downloaded at configure time.

```powershell
$env:DAW_QT_DIR = "C:/Qt/6.8.3/msvc2022_64"   # your Qt install
cmake --preset vs2022
cmake --build --preset release
ctest --preset release
```

The engine, project and AI layers build without Qt:

```powershell
cmake --preset vs2022-headless
cmake --build --preset headless-debug
ctest --preset headless-debug
```

A git-ignored `CMakeUserPresets.json` can hard-code your Qt path (for example `local`, `local-debug`, `local-release` presets).

<details>
<summary>Build options</summary>

| Option | Default | Meaning |
|--------|---------|---------|
| `DAW_BUILD_APP` | ON | Build the Qt desktop app |
| `DAW_BUILD_TESTS` | ON | Build the Catch2 test suite |
| `DAW_ENABLE_ONNX` | ON | ONNX Runtime backend (OFF builds a stub that reports *Unavailable*) |
| `DAW_ONNXRUNTIME_VERSION` | 1.22.0 | NuGet `Microsoft.ML.OnnxRuntime` version to download |
| `DAW_ONNXRUNTIME_ROOT` | (empty) | Use an extracted ORT package instead, e.g. the DirectML package |
| `DAW_ENABLE_ASIO` / `DAW_ASIO_SDK_DIR` | OFF | ASIO device type (Steinberg ASIO SDK under its GPL-3.0 option) |
| `DAW_WARNINGS_AS_ERRORS` | OFF | `/WX` for first-party targets |

</details>

## Using it

`build/<preset>/bin/<Config>/StemDAWg.exe [project-folder | audio files…]`

- **Import:** Ctrl+I or drag and drop WAV, FLAC, AIFF, OGG, MP3 or MIDI. Numbered Suno stems ("0 Lead Vocals", "1 Backing Vocals", …) stay in order. Files imported together lose the text they all share ("Song_ (Bass Guitar)" becomes "Bass Guitar").
- **MIDI:** the first MIDI import offers to download the FluidR3_GM SoundFont (114 MB, MIT licence, from Debian, checked with SHA-256). Any other `.sf2` works too: Settings → SoundFont for MIDI. MIDI tracks are labelled "(MIDI)".
- **Transport:** Space plays and pauses; Stop returns to the start; Home jumps to the start; click the timeline to seek.
- **Selecting time:** drag across the tracks to select a stretch of time (Shift+click extends it, Esc or a click clears it). Dragging on a track also shows it in the Clean-up panel.
- **Navigation:** the wheel scrolls tracks (Shift for sideways), Ctrl+wheel zooms.
- **Undo:** Ctrl+Z undoes any edit (faders, mute, solo, clean-up, imports, renames, deletions); Ctrl+Y redoes. A fader drag is one step.
- **Track headers:** double-click the name to rename; right-click for Rename, Clean-up and Delete Track. C opens the track's clean-up (blue when on); M mutes; S solos that track alone (Ctrl+click S to solo several; mute wins); the fader sets volume (double-click for 0 dB); the knob pans (double-click to centre).
- **Clean-up** (right panel, Ctrl+L): bleed suppression (pick a strength, then This Track or All Stems; Original switches back), voice denoise for speech (Apply renders a cleaned copy, Original switches back; the first use downloads the model), high-pass, hum removal (Detect finds it), noise reduction (Learn Noise first; if the track's quietest moments are not the noise you want gone, select a stretch of pure noise and click Learn from Selection), stereo (mono below a frequency, width, align phase), harshness (the most to cut at 2–6 kHz and at 8–17 kHz), expander, transients (attack and sustain) and DC removal for the selected track, with a gain-reduction meter. **Listen to Removed** (Ctrl+Shift+B) plays only what the clean-up removes: if you hear music in it, back off. **Apply Fix** / **Apply All Fixes** in the stem report fill it in from the advice. **Bypass Clean-up** (Ctrl+B) plays everything without it, for comparison; exports always include it.
- **Master and CLIP:** Suno stems often add up above 0 dBFS. Open the **Master** panel (Ctrl+M), pick a loudness target and click **Match Loudness**: it switches on the limiter and sets the Master fader. Or pull the Master fader down until the CLIP light stays off. **Tonal balance** in the same panel: **New Target...** (a folder of reference tracks), **Compare**, **Apply Suggested EQ**.
- **Settings → Artist Profiles**, then **File → Export Mix** (Ctrl+E).
- **Stem Report** (bottom panel) runs on its own after import. Run it again with **Analyze** or Ctrl+R after changing faders. Click an advice item to find its stem.
- **Loudness** (transport bar): M, S and I are momentary, short-term and integrated LUFS; TP is the true peak. Play the song from the start for its integrated loudness; click the readout to restart the measurement.
- **Analysis → Detailed File Analysis** (Ctrl+Shift+A) measures each file in detail (Analysis Log tab); transients show as yellow markers.
- **Command line:** `stem-report <folder of stems> [--json] [--apply-fixes] [--denoise[=dB]] [--bleed[=light|medium|strong]] [--out=folder]` prints the same stem report without the app; `--apply-fixes`, `--denoise` and `--bleed` also show the stems after the suggested clean-up, noise reduction or bleed suppression (`--out` keeps the bleed-suppressed copies to listen to). `voice-denoise <model folder> <input> <output.wav> [--reduction=dB]` runs the voice model on one file. `drums-to-midi <drums.wav> --out=drums.mid [--grid=16|32|0]` transcribes a drum stem; `notes-to-midi <stem.wav> --out=part.mid [--beats=drums.wav] [--grid=16]` a pitched one.
- **Data locations:** profiles in `%LOCALAPPDATA%\StemDAWg\profiles.json`, logs in `%LOCALAPPDATA%\StemDAWg\logs`, the SoundFont in `%LOCALAPPDATA%\StemDAWg\SoundFonts`.

## Documentation

- [Roadmap](docs/ROADMAP.md): cleaning, noise removal, AI-artifact repair, mastering, audio-to-MIDI
- [Milestone 2: stem triage, loudness and clean-up](docs/milestone-2/M2-Stem-Triage.md): how the stem report and the meters measure, and how the clean-up chain works
- [MIDI playback](docs/milestone-2/MIDI-Playback.md): SoundFont rendering, the FluidR3_GM download
- [Editing and playback](docs/milestone-2/M2-Editing-and-Playback.md): undo/redo, track editing, declick, RF64 export
- [Milestone 3: noise removal](docs/milestone-3/M3-Noise-Removal.md): spectral noise reduction, hum removal, voice denoise, Listen to Removed
- [Milestone 4: AI-artifact repair](docs/milestone-4/M4-Artifact-Repair.md): bleed suppression between stems, transient restoration, harshness and shimmer control, stereo repair
- [Milestone 5: Mastering](docs/milestone-5/M5-Mastering.md): the master bus, true-peak limiter and loudness matching, tonal balance against reference tracks, soft clipper and multiband compressor, export presets per destination
- [Milestone 6: Audio to MIDI](docs/milestone-6/M6-Audio-to-MIDI.md): drums to MIDI, notes to MIDI, beat and downbeat tracking, exact MIDI timing
- [Milestone 1 technical specification](docs/milestone-1/M1-Technical-Specification.md): architecture, real-time rules, file formats
- [Implementation sequence](docs/milestone-1/M1-Implementation-Sequence.md)
- [Contributing](CONTRIBUTING.md)

## Source layout

```
core/          RT primitives, ids, errors, logging, files      (no JUCE/Qt)
dsp/           loudness meter, true peak, filters, FFT         (no JUCE/Qt)
media/         decode, WAV writer + tags, resampler, peaks     (JUCE private)
engine/        transport, audio engine, device service         (JUCE private)
project/       model, project.json + migrations, profiles      (no JUCE/Qt)
session/       media loading, snapshots, import, render/export, stem report
ai/            analysis service, analyzers, ONNX backend       (no JUCE/Qt)
apps/desktop/  Qt application
tools/         command-line tools: stem-report, voice-denoise, drums-to-midi, notes-to-midi
assets/        logo and icon masters
tests/         unit, regression and integration tests
```

## License

Created by Craig "Deverydoo" Giannelli.
Copyright © 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors.

Stem DAWg is free software under the [GNU Affero General Public License v3.0 or later](LICENSE). It uses JUCE (AGPL-3.0), Qt (LGPL-3.0, dynamically linked), ONNX Runtime (MIT), nlohmann/json (MIT), TinySoundFont (MIT), XZ Embedded (0BSD) and Spotify's Basic Pitch model (Apache-2.0). The optional FluidR3_GM SoundFont (MIT) is downloaded at run time. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The Stem DAWg name, logo and icon are the project's branding. Forks should use their own name and artwork.
