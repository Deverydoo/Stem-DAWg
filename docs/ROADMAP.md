# Stem DAWg Roadmap

## What this project is

A **music cleaner and mastering tool**, not a general-purpose DAW. It takes audio that sounds wrong and makes it sound right:

- AI-generated songs and their stems (Suno and similar), which carry separation bleed, noise beds, smeared transients and harsh, phasey high ends.
- Home recordings made without a studio: fan and air-conditioning noise, room hum, laptop hiss.

The goal is **clean, balanced, release-ready audio**. Industrial, EBM and techno come first; other genres follow as the analysis and targets mature.

**Not goals (for now):** creative effects racks, synth instruments, MIDI sequencing, or cloning Audition, Ableton or Pro Tools. Plugin hosting may come later, but only where it serves cleaning and mastering.

## What real Suno stems look like

A 9-stem Suno export (6:21, 48 kHz / 16-bit stereo, stems: lead vocals, backing vocals, drums, bass, guitar, keyboard, percussion, synth, other) was measured with simple level, spectrum and correlation analysis. The findings drive the priorities below.

| Finding | Evidence | Consequence |
|---------|----------|-------------|
| **Stems sum far above full scale** | Sum of all 9 stems at 0 dB peaks at **+5.0 dBFS** | Playback and renders clip unless the mix is pulled down. Master fader and clip warning shipped; loudness-aware auto gain staging and a true-peak limiter are needed. |
| **Constant noise/bleed beds** | When "silent", bass sits at **−50 dBFS**, synth at −50, drums at −54, "other" at −50 (lead vocal is clean at −69) | Every stem plays a bed of residue under the music. Needs per-stem expansion/gating and spectral denoise using the stem's own quiet parts as the noise profile. |
| **"Other" is mostly residue** | Active 69% of the time but only −17.5 dB of the mix; envelope correlates with drums, percussion and synth (0.30–0.36) | Often safe to mute or heavily gate. A triage report should say so automatically. |
| **Percussion duplicates the kick** | 58% of its energy is sub-bass (< 60 Hz); correlated with drums (0.26) | Doubled kick in the low end causes phase smear and mud. High-pass non-drum stems or remove the duplicated band. |
| **Bass leaks into synth; keyboard and synth overlap** | Synth vs bass sample correlation 0.42; keyboard vs synth 0.39; synth carries 32% bass-band energy | Low-end ownership: only bass and kick should own the sub. Per-stem high-pass and cross-stem bleed suppression. |
| **Lead vocal bleeds into backing vocals** | Correlation 0.42 | Backing-vocal cleanup using the lead stem as a reference. |
| **Very wide, phasey stems** | Keyboard L/R correlation 0.24 | Mono-compatibility check and stereo repair, especially in the lows. |
| Full bandwidth, no DC problems | All stems reach ~20 kHz; DC under −70 dB | No band-limit repair needed for this source. |

The key opportunity: **we have every stem**. Bleed of stem A inside stem B can be estimated from A itself (cross-stem spectral masking), which single-file denoisers cannot do.

## AI assistance (across every milestone)

AI is part of the product from the start (ONNX Runtime and a background analysis service already ship). Models **advise and automate the tedious parts; you stay in control**: every AI suggestion appears as normal, editable settings you can audition, A/B and undo.

- **Analyse each stem:** classify what a stem really contains (is "Other" just residue? does "Percussion" hide a second kick?), estimate its noise floor, find artifacts (bleed, shimmer, smeared transients, clipping) and mark where they happen on the timeline.
- **Suggest the adjustments:** propose filter cutoffs, gate/expander thresholds, denoise strength, EQ moves and stem levels from the analysis. Example: "high-pass Synth at 110 Hz, it carries the bass's low end", or "expander on Bass, threshold −48 dB, its bed sits at −50 dB".
- **Do the heavy lifting:** neural denoising (speech and music), re-separation or bleed suppression when Suno's split is poor, transient restoration.
- **Tuning:** pitch detection per stem for vocal tuning and for checking that bass, synth and keys agree on key and intonation.
- **Mastering targets:** compare the mix with genre references and suggest tonal-balance and loudness moves.

Models run locally via ONNX Runtime (CPU, with DirectML GPU acceleration planned). No audio leaves your computer. Every bundled model must have an AGPL-compatible licence and documented training-data provenance (see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).

## Milestones

Milestone 1, the engineering foundation, is done: the engine, projects, waveforms, track mixer, export with metadata, and the analysis and ONNX framework. See [milestone-1](milestone-1/).

### M2: Stem triage and clean-up basics
- **Stem report** (automatic after import): level share, active time, noise floor, low-end content, bleed against every other stem, with plain-language advice ("Other is mostly residue: consider muting"; "Percussion doubles the kick below 60 Hz").
- **Per-stem clean-up chain, non-destructive:** high-pass / low-end ownership, downward expander with its threshold set from the measured noise floor, DC removal.
- **Loudness metering:** integrated, short-term and momentary LUFS (ITU-R BS.1770-4), true peak, LRA; shown live and in the export report.
- Delete and rename tracks; undo/redo (command pattern, already prepared by the model design).
- Declick on stop/seek; RF64 export for renders over 4 GB.

### M3: Noise removal
- **Learned-profile spectral denoise** for stationary noise (fans, AC, hiss): the profile comes from quiet regions (auto-detected, or user-selected), applied as a smoothed Wiener/spectral-subtraction gain with protection against musical noise.
- **Hum removal:** 50/60 Hz fundamentals and harmonics with adaptive notches.
- **Voice denoise model** (ONNX): evaluate DeepFilterNet3 (MIT/Apache-2.0, 48 kHz full-band) and RNNoise (BSD) for spoken and sung home recordings.
- **Listen to what's removed:** a delta/difference monitor and A/B against the original, so cleaning never damages the music unnoticed.

### M4: AI-artifact repair
- **Cross-stem bleed suppression:** estimate each stem's leakage into the others and mask it.
- **Transient restoration** for smeared drums: re-sharpen attacks using onset detection, which already exists.
- **Harshness and "shimmer" control:** dynamic EQ in the 2–6 kHz and 8–16 kHz bands, driven by spectral analysis rather than static cuts.
- **Stereo repair:** mono below ~120 Hz, correlation-guided width correction.

### M5: Mastering
- Genre tonal-balance targets (industrial, EBM, techno first) compared against the mix, with suggested EQ moves.
- Multiband compression, a true-peak limiter, and loudness targets (streaming −14 LUFS; club/techno masters typically louder) with −1 dBTP ceilings.
- Reference-track matching (load a commercial track, compare spectrum and loudness).
- Export presets per destination (streaming, Bandcamp, club, CD).

### M6: Audio to MIDI
- **Drums first:** onsets (existing) → hit classification (kick / snare / clap / hats / toms from band-energy features, then an ML drum-transcription model) → quantise to a detected tempo grid → General MIDI drum file (`.mid`). This lets you rebuild a Suno beat with real drum samples.
- **Tempo and grid detection** (also improves the bar.beat display).
- **Pitched lines:** bass and synth via Spotify's Basic Pitch (Apache-2.0, ONNX-exportable), then polyphonic models for keys.

### Later
- VST3 hosting for third-party restoration and mastering tools.
- Sample-accurate automation.
- GPU waveform rendering, disk streaming for very long sessions.

## Open-source readiness
- Licence: **AGPL-3.0-or-later** (decided). JUCE 8's open-source option is AGPL-3.0; every other dependency (Qt LGPL-3.0, ONNX Runtime MIT, nlohmann/json MIT, Catch2 BSL-1.0, ASIO SDK GPL-3.0 option, VST 3 SDK MIT) is compatible. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
- CI (Windows build and tests on every push), contribution guide, issue templates.
- Keep test audio generated in code: no copyrighted or private audio in the repository.
