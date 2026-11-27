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

### M2: Stem triage and clean-up basics ✅ done
- **Stem report** (automatic after import) ✅ **done:** level share, loudness, active time, noise floor, low-end content, stereo, and per-band correlation against every other stem, with plain-language advice ("Other holds what the separation could not assign: mute it and listen"; "Percussion is mostly low end under Drums: it probably carries a second kick"). Also available as the `stem-report` command-line tool. Design: [M2-Stem-Triage.md](milestone-2/M2-Stem-Triage.md).
- **Loudness metering** ✅ **done:** integrated, short-term and momentary LUFS (ITU-R BS.1770-4), true peak, LRA; live in the transport bar, in the stem report and in the export summary.
- **Per-stem clean-up chain, non-destructive** ✅ **done:** DC removal, Butterworth high-pass (low-end ownership) and a lookahead downward expander per track, live and saved with the project. Stem report advice carries the fixes (expander threshold from the measured noise bed, high-pass for doubled low end), applied with one click; Bypass Clean-up (Ctrl+B) compares with the original. See [M2-Stem-Triage.md](milestone-2/M2-Stem-Triage.md#3-clean-up-chain-dspcleanupchain).
- **Delete and rename tracks; undo/redo** ✅ **done:** every edit is undoable (model snapshots, with gestures such as a fader drag or a batch import merged into one step). See [M2-Editing-and-Playback.md](milestone-2/M2-Editing-and-Playback.md).
- **Declick on stop/seek; RF64 export for renders over 4 GB** ✅ **done.**

### M3: Noise removal
- **Learned-profile spectral denoise** ✅ **done** for stationary noise (fans, AC, hiss): the profile comes from the quietest frames, or from a stretch of noise selected on the timeline, applied as a decision-directed Wiener gain that avoids musical noise. See [M3-Noise-Removal.md](milestone-3/M3-Noise-Removal.md).
- **Hum removal** ✅ **done:** 50/60 Hz fundamentals and harmonics with narrow notches, detected automatically (frequency-checked, so bass notes near 50 Hz are not mistaken for hum).
- **Voice denoise model** ✅ **done** (ONNX, DeepFilterNet3, MIT/Apache-2.0, 48 kHz full-band): renders a cleaned copy of a spoken recording, removing noise that changes as well as steady noise; the original stays in the project. Measured: excellent on speech, but it takes a wet AI singing voice for noise, so the app warns when the voice drops and steers singing to the spectral denoiser. A singing-capable model remains an open item.
- **Listen to what's removed** ✅ **done:** Listen to Removed (Ctrl+Shift+B) plays exactly what the clean-up takes away; Bypass Clean-up (Ctrl+B) is the A/B.

### M4: AI-artifact repair ✅ done
- **Cross-stem bleed suppression** ✅ **done:** every stem is judged against all the others. Its share of each time-frequency bin is sharpened, so it is turned down where another stem owns the sound; bins that stems genuinely share are left alone. It renders processed copies (non-destructive, undoable). On a 9-stem Suno export the noise beds between notes dropped by 11–19 dB, while the music changed by 0.1–1.6 dB. See [M4-Artifact-Repair.md](milestone-4/M4-Artifact-Repair.md).
- **Transient restoration** ✅ **done:** the stem report measures how fast each stem's hits rise (Suno drums: about 3 ms; a sampled drum: about 1 ms) and suggests a transient shaper for soft drum stems. The shaper lifts the slow start of each hit towards the level it is heading for, using the chain's lookahead, so hits rise faster without growing louder; sustain shapes the decay. See [M4-Artifact-Repair.md](milestone-4/M4-Artifact-Repair.md#2-transient-restoration-dsptransientshaper-stem-report-attack).
- **Harshness and "shimmer" control** ✅ **done:** measured on 24 Suno songs, the harshness is narrow: partials that ring through many notes at 2–4 kHz, and steady generator tones at fixed frequencies (8, 16, 16.6, 16.9 kHz). A resonance suppressor in the clean-up chain's spectral stage turns down peaks that persist above the spectrum around them, in 2–6 kHz and 8–17.5 kHz, only while they ring and never below their surroundings; the stem report finds them and suggests it. See [M4-Artifact-Repair.md](milestone-4/M4-Artifact-Repair.md#3-harshness-and-shimmer-control-dspresonancesuppressor-stem-report-harsh--shimmer).
- **Stereo repair** ✅ **done:** separated stems carry a wide or out-of-phase low end, and some Suno songs carry the same sound in both channels out of step, so a mono sum loses up to 3.6 dB in the midrange. The clean-up chain gains mono bass (side high-passed below 120 Hz), width, and coherence-guided phase alignment, which brings the channels into step without changing their levels; the stem report measures the low end and the mono sum per range and suggests each fix where it helps. See [M4-Artifact-Repair.md](milestone-4/M4-Artifact-Repair.md#4-stereo-repair-dspstereo-stereoimager-phasealigner).

### M5: Mastering ✅ done
- **Tonal-balance targets with suggested EQ moves** ✅ **done:** no genre curves are published that could be shipped, so targets are built from reference tracks the user owns (a folder of industrial, EBM or techno releases): their average per third-octave band and how much they disagree. The mix is compared with the target and a five-band master EQ is fitted to the difference; the prediction is what measuring again reports. See [M5-Mastering.md](milestone-5/M5-Mastering.md#2-tonal-balance-against-reference-tracks-and-the-master-eq-dsptonalbalance).
- **True-peak limiter and loudness targets** ✅ **done:** a master bus after the summed tracks with a 4x-oversampled true-peak limiter (never more than 0.02 dB over its ceiling on stress signals), and Match Loudness, which sets the master fader for a target (streaming −14, Apple Music −16, CD/Bandcamp −10, club −8 LUFS) through the limiter without asking it for more than 12 dB. See [M5-Mastering.md](milestone-5/M5-Mastering.md).
- **Louder masters** ✅ **done:** measured, a multiband compressor does not make this kind of mix louder (it only moves work from the limiter); what holds a loud master back is the millisecond peak-to-body difference no gain envelope can touch. So the master bus gains a 4x-oversampled **soft clipper** (−8.6 LUFS from a mix the limiter alone held at −9.5, with more dynamics left), used by Match Loudness within a 2 % budget, and a three-band **multiband compressor** for control. See [M5-Mastering.md](milestone-5/M5-Mastering.md#3-louder-masters-the-multiband-compressor-and-the-soft-clipper-dspmultiband-dspclipper).
- **Reference-track matching** ✅ **done** as part of tonal targets (a target from one track is a match to it); loudness has Match Loudness.
- **Export presets per destination** ✅ **done:** Export Mix masters a copy of the project for streaming (−14 LUFS, −1 dBTP, 24-bit), Bandcamp (−10 LUFS), club (−8 LUFS, up to 48 kHz) or CD (−10 LUFS, −0.3 dBTP, 16-bit 44.1 kHz), or all four in one go (88 s for a 6-minute, 9-stem song), each by Match Loudness at the file's own rate; the project is not changed. See [M5-Mastering.md](milestone-5/M5-Mastering.md#4-export-presets-per-destination-sessiondestinations).

### M6: Audio to MIDI
- **MIDI playback** ✅ **done early:** `.mid` files import as tracks rendered through a General MIDI SoundFont (FluidR3_GM, downloaded in the app), so Suno's own MIDI (advanced stem split) can be compared with, layered on, or used to replace the audio stems. See [MIDI-Playback.md](milestone-2/MIDI-Playback.md).
- **Drums to MIDI** ✅ **done** (DSP; an ML model may follow): a drum stem becomes a General MIDI file (kick, snare, closed and open hi-hat, crash, toms) on the song's beats, quantised within each beat, with a tempo change on every beat so it stays on the audio. Instrument templates are learned from the song's own hits and fitted together (non-negative least squares), hats and crashes told apart by their ring. Measured on Suno's drum MIDI rendered through the SoundFont: kick F 0.97, hat 0.92, snare 0.84, crash 0.75; on the real stem, kicks 0.86. Toms ✅ found by their ring and pitch against the song's own kicks and snares, sorted into floor, low and high (F 0.87-0.94 on four of six GM kits; all 18 on Suno's MIDI render). See [M6-Audio-to-MIDI.md](milestone-6/M6-Audio-to-MIDI.md).
- **Tempo and beat tracking** ✅ **done** for drums: Suno songs drift (135 → 138 BPM and back within one song), so the beats are tracked on the drum hits by dynamic programming that follows the local tempo (beats F 0.88-0.93), with downbeats decided beat by beat (80-83 % of bars). The beats are kept in the project: the timeline's ruler shows the song's bars and beats, with bar lines across the tracks, and the position follows them (Analysis → Track the Beats) ✅. A window with little rhythm keeps the song's tempo, which fixed intros and breaks tracked 10-15 % fast (beats F 0.88 → 0.93, downbeats 80 → 87 %). Still open: beats for songs without drums.
- **MIDI timing fixed:** the MIDI loader used for playback (TinyMidiLoader) rounded at every tempo change, so an imported Suno MIDI file drifted 0.34 s early over a 6-minute song; our own reader replaces it.
- **Notes to MIDI** ✅ **done**: pitched stems (bass, guitars, keys, synths, voices) become MIDI with Spotify's Basic Pitch (Apache-2.0, 230 KB, compiled in), on the song's beats when there is a drum track, the instrument chosen from the track name. Measured on Suno's MIDI parts rendered through the SoundFont (notes within 50 ms, octave doublings counted once): bass 0.75, synth keys 0.75, electric piano 0.82, guitars 0.64-0.70, voices 0.51-0.54. Still open: pitch bends (the model gives them), a model trained for singing.
- Snapping selections to the tracked beats; beats for songs without drums; pitch bends in Notes to MIDI.

### Later
- VST3 hosting for third-party restoration and mastering tools.
- Sample-accurate automation.
- GPU waveform rendering, disk streaming for very long sessions.

## Open-source readiness
- Licence: **AGPL-3.0-or-later** (decided). JUCE 8's open-source option is AGPL-3.0; every other dependency (Qt LGPL-3.0, ONNX Runtime MIT, nlohmann/json MIT, Catch2 BSL-1.0, ASIO SDK GPL-3.0 option, VST 3 SDK MIT) is compatible. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
- CI (Windows build and tests on every push), contribution guide, issue templates.
- Keep test audio generated in code: no copyrighted or private audio in the repository.
