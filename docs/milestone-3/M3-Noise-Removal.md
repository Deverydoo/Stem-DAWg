# Stem DAWg — Milestone 3: Noise removal

New tools for the noise a home recording or an AI stem carries:
- **Noise reduction** learns the noise and removes it everywhere.
- **Hum removal** notches out mains hum.
- **Voice denoise** (a neural network) cleans spoken recordings, including noise that keeps changing.
- **Listen to Removed** plays what the clean-up takes away, so cleaning never damages the music unnoticed.

The chain is now: (voice denoise, rendered) → DC removal → high-pass → **hum removal** → **noise reduction** → expander.

## 1. Spectral noise reduction (`dsp/SpectralDenoiser`)

**What it removes.** Steady noise: fans, air conditioning, hiss, the constant part of a separation bed. Noise that changes from moment to moment, such as the bleed of other instruments in an AI stem, is the expander's job.

**How.**
- **Frames:** short-time Fourier transform with 2048-point frames at 75 % overlap, using square-root Hann analysis and synthesis windows (perfect reconstruction).
- **Stereo:** both channels share one complex FFT, with left as the real part and right as the imaginary part. One real gain per bin is applied to both, so stereo costs one forward and one inverse transform per hop and the stereo image is kept.
- **Gain per bin:** a Wiener gain from the decision-directed a-priori SNR (Ephraim and Malah, 1984). It follows the music without the warbling "musical noise" plain spectral subtraction leaves. Attenuation never exceeds the *Reduction* setting (default 12 dB), and *Sensitivity* (±10 dB) scales the profile.
- **Real-time safety:** no allocation after `prepare()` (tested).

**Learning the profile (`session/NoiseTools`).**
- *Learn Noise* renders the track as it enters the denoiser: unity gain, after DC removal, the high-pass and hum removal.
- It takes the quietest 10 % of the frames that are not digital silence: the room between phrases in a recording, the bed between notes in a stem.
- **From a selection:** when the quietest moments are not the noise to remove (a fan that only runs for part of the take, a quiet room tone at the end), drag across a stretch of pure noise on the timeline and click *Learn from Selection*. Every non-silent frame wholly inside the selection is used. It needs a little over 0.25 s of noise; the profile remembers that it came from a selection.
- It averages their spectra, windowed exactly as the denoiser windows its frames.
- **Two passes:** the first measures each frame's level in the time domain (Parseval's theorem: no FFT needed). The second transforms only the quiet frames. A 6.6-minute stem learns in about 0.3 s.
- **Storage:** the profile is saved with the project, at 0.01 dB resolution. At another engine sample rate it is resampled by frequency.

**Measured.** On a real lead-vocal stem with steady fan-like noise at -50 dBFS added:

| Setting | Noise between phrases | Vocal loudness |
|---|---|---|
| none | -50.0 dBFS | -18.6 LUFS |
| 12 dB | -62.0 dBFS | -18.7 LUFS |
| 20 dB | -70.0 dBFS | -18.7 LUFS |

On the Suno stems themselves the reduction is smaller, 1–4 dB, because their beds are mostly bleed rather than steady noise. There the expander does more.

## 2. Hum removal (`dsp/HumFilter`, `session/NoiseTools`)

**The notches.** Up to eight notches at the mains frequency and its harmonics. Each notch is a constant 3 Hz wide, so the harmonics are cut as narrowly as the fundamental and the music barely notices. Each harmonic is cut by more than 40 dB, and 1 kHz is left within 0.05 dB (tested).

**Detection.** *Detect* (or *Detect in Selection*, over every frame of a time selection) averages the quiet frames with 16384-point frames, which gives 2.9 Hz bins at 48 kHz, enough to tell 50 Hz from 60 Hz. A harmonic counts when both of these hold:
- **Level:** it stands at least 12 dB above the median of the spectrum 10–35 Hz around it.
- **Frequency:** its interpolated frequency lies within 0.5 Hz + 0.25 Hz per harmonic of the exact multiple.

The frequency check matters. Mains hum sits on exact multiples, while musical notes rarely do: G1 is 49.0 Hz. Without it, quiet bass and keyboard notes on a Suno song were reported as 50 Hz hum. With it, none are, and real hum at 50 or 60 Hz is still found (tested, including a G1 bass note that must not count).

## 3. Listen to Removed

The chain keeps its input delayed by its full latency. With *Listen to Removed* (Ctrl+Shift+B) it outputs that dry input minus its result, which is exactly what the clean-up removes (tested to 1e-6). Tracks without a chain are silent while it is on.
- If music can be heard in it, the settings are too strong.
- It is a listening aid: not saved, never in exports.
- It is exclusive with *Bypass Clean-up*, since both together would be silence.

## 4. Voice denoise (`ai/VoiceDenoiser`, `dsp/DeepFilterDsp`, `session/VoiceTools`)

**What it removes.** Anything that is not speech: fans, traffic, keyboards, room tone, noise whose level or colour keeps changing, which a fixed noise profile cannot follow. It uses **DeepFilterNet3** (Schröter et al., 2023; MIT or Apache-2.0), a small network that runs on the computer through ONNX Runtime.

**The model.** An optional 8 MB download (the first *Apply* offers it). The ONNX export is pinned to a commit of the DeepFilterNet repository and verified by SHA-256: the archive and each of its three graphs. It is installed in `%LOCALAPPDATA%\StemDAWg\Models\DeepFilterNet3`.

**How.**
- **Signal processing in C++** (`dsp/DeepFilterDsp`), mirroring the reference implementation exactly, because the network was trained on these features:
  - 960-point STFT frames at hop 480 with a Vorbis window, using an any-size FFT (Bluestein's chirp-z, `ChirpFft`);
  - log power in 32 ERB bands, mean-normalised;
  - the lowest 96 bins as a unit-normalised complex spectrum.
- **The network** (three graphs: encoder, ERB-mask decoder, deep-filter decoder) returns band gains for the whole spectrum and a 5-tap complex filter across frames for the lowest 96 bins, where speech harmonics live.
- **Offline, in chunks:** the graphs process whole sequences, so features are computed for the whole channel first. Inference then runs on chunks of 10 s, each with 1 s of warm-up so the network's recurrent state settles. Memory stays small for long songs.
- **Speed:** about 24 times real time on the CPU. A minute of stereo takes about 3 s.
- **Reduction:** the model's attenuation limit (6–24 dB or Full) mixes some of the input back.
- Three choices were settled by measurement on speech, each with a clear optimum: the output alignment (2 frames, the network's lookahead), applying the deep filter to the unmasked spectrum, and the coefficient layout.

**Non-destructive, but rendered.** The model cannot run live (inference is not allowed on the audio thread), so *Apply* renders a cleaned copy of the track's **original** recording into `Media/Processed`. The track's clips then play the copy, and *Original* switches back. Both are undoable, and the original never leaves the project. Re-applying at another strength renders from the original again, never processing twice.

**Measured** (60 s of speech at -20 dBFS with noise added):

| Input | Output SNR | Noise between phrases | Voice level |
|---|---|---|---|
| clean | 30.5 dB | — | -0.25 dB |
| steady fan noise (SNR 18.1 dB) | 22.6 dB | -40 → -59 dBFS | -0.19 dB |
| fan speeding up and slowing down, plus keyboard clicks (SNR 18.8 dB) | 22.8 dB | → -60 dBFS | -0.21 dB |

In the app, on the second file, the stem report's noise bed fell from -48.2 to -68.6 dBFS.

**Not for singing.** On a Suno lead-vocal stem the model takes the wet, effected singing voice for noise: the vocal drops by 8 dB, and by 4 dB even with a 6 dB reduction limit. So:
- the section is labelled for speech;
- after each run the app compares the loudest half of the recording before and after, and warns when the voice dropped by more than 2 dB;
- singing goes to *Noise reduction*.

**Command line.** `voice-denoise <model folder> <input> <output.wav> [--reduction=dB]` runs the model on one file.

## 5. Time selection

Dragging across the lanes now selects a stretch of time; dragging on the ruler still scrubs the playhead, and a plain click still seeks (and clears the selection). Shift+click extends the selection from its far end, or from the playhead; Esc clears it. Releasing a drag moves the playhead to the selection's start, and pressing on a lane shows that track in the Clean-up panel, so *select the noise, click Learn* is one motion. The selection is a view state: not saved, not undoable. Later milestones use it for loops and region edits.

## 6. Engine changes

- **Fixed latency:** the chain's latency is now the FFT frame plus the expander lookahead: 2048 + 240 frames, 48 ms at 48 kHz, whichever stages are on. The engine keeps hiding it by reading processed tracks ahead. When noise reduction is off, a plain delay stands in for it.
- **Restart on toggle:** switching noise reduction on or off restarts the track's chain at the next block, re-primed from the audio before the playhead.
- **Profiles in the snapshot:** each profile is resampled to the engine rate on the control thread. The snapshot owns it, and the audio thread copies it into the chain's preallocated buffer.
- **Memory:** a chain now preallocates about 140 KB. `EngineConfig::maxCleanupTracks` sets how many tracks can run one: 128 live, one per track in renders and the stem report.

## 7. Desktop app

- The **Clean-up** panel has *Hum removal* (50/60 Hz, harmonics, Detect) and *Noise reduction* (Learn Noise, Reduction, Sensitivity) sections.
- *Listen to Removed* is in the panel, the Clean-up menu and the toolbar.
- *Learn Noise* and *Detect* run in the background and finish as one undoable step each.
- The `stem-report` tool's `--denoise[=dB]` option learns every stem's profile, checks it for hum, and reports the stems again after cleaning.

## 8. Tests

- `daw_dsp_tests` `[denoise]` / `[hum]` / `[delta]`:
  - reconstruction without a profile, delayed by exactly one frame;
  - stationary noise reduced by the set amount (12 ± 1 dB);
  - a tone 37 dB above the noise kept within 0.3 dB while the noise around it drops by over 17 dB at a 20 dB setting;
  - the hum notches at 50 and 60 Hz;
  - listen-to-removed equal to input minus output;
  - the whole chain allocation-free;
  - the chain latency (frame plus lookahead).
- `daw_integration_tests` `[noise]`:
  - a learned profile within 0.5 dB of the analytic white-noise level;
  - learning from a selected region of loud noise instead of the quieter room elsewhere, and refusing selections that are too short, past the end or silent;
  - hum detection inside a selection, and none in a selection without hum;
  - noise reduction lowering the bed measured by the stem report by 12 ± 1.5 dB, with the notes untouched;
  - hum detection at 50 and 60 Hz, none in clean audio, none for a G1 bass note;
  - no hum left after hum removal;
  - refusal of silent and missing tracks;
  - profile resampling across rates;
  - listen-to-removed in the snapshot.
- `daw_project_tests`: hum and noise-reduction settings, including a profile, round-trip through the project file.
- `daw_dsp_tests` `[deepfilter]` / `[fft]`:
  - the chirp-z FFT against a direct DFT at sizes 1 to 960, inverting exactly and allocation-free;
  - the ERB band layout against libDF's;
  - the Vorbis window's perfect reconstruction (analysis plus synthesis equals the input one hop late) and the reference spectrum scaling;
  - the feature normalisation, band gains and deep filter.
- `daw_media_tests` `[archive]`: `.tar.gz` extraction, with GNU long names, all or nothing, a size limit, and damaged input.
- `daw_integration_tests` `[voice]`:
  - processed copies: apply, re-apply at another strength, go back to the original;
  - pruning, validation and the project-file round trip;
  - the model package refusing a wrong download;
  - with the model installed, a real run: noise reduced by over 15 dB, the reduction limit respected, rate and length kept, MIDI refused. It is skipped when the model is not installed.

## Still open

- **Singing:** a denoise model that keeps sung and effected vocals intact.
- ~~**Disk space:** processed copies that no clip plays any more stay in `Media/Processed`.~~ Done in M4: they are deleted when the project is closed (see [M4](../milestone-4/M4-Artifact-Repair.md#disk-space)).
