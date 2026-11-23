# Stem DAWg — Milestone 2: Stem triage, loudness and clean-up

This note covers the first two parts of Milestone 2: the **stem report** and **loudness metering**. Both only analyse; they never change audio. The clean-up chain that acts on their findings comes next.

## 1. Loudness metering (`dsp/LoudnessMeter`)

One implementation serves three places: the live meter in the transport bar (on the audio thread), the stem report (one meter per stem plus the mix), and the export summary.

| Measure | Definition |
|---------|------------|
| Momentary | Mean square of the K-weighted signal over the last 400 ms, updated every 100 ms |
| Short-term | The same over the last 3 s |
| Integrated | Mean of the 400 ms blocks (75 % overlap) above the absolute gate (-70 LUFS) and the relative gate (10 LU below the mean of the absolutely gated blocks) |
| Loudness range (LRA) | 95th minus 10th percentile of the short-term values above -70 LUFS and 20 LU below their mean (EBU Tech 3342) |
| True peak | Peak of the signal oversampled 4x below 96 kHz (2x below 192 kHz) with a 64-tap Kaiser-windowed sinc interpolator, never below the sample peak |

Loudness is `-0.691 + 10 log10(sum of the channels' mean squares)`, with channel weight 1.0. The K-weighting filter is re-derived for every sample rate from its analogue prototype; at 48 kHz it reproduces the BS.1770-4 coefficients to 1e-7, which is tested.

**Real-time design.** `prepare()` allocates; `process()`, `reset()` and `reading()` do not (tested with the allocation guard). The gating statistics are histograms of 4,000 bins, each 0.02 LU wide, from -70 to +10 LUFS. Each bin holds the **exact sum of block energies** and a block count. The gated means are therefore exact; only a block within 0.02 LU of a relative gate can be classified on the wrong side of it. Memory stays fixed for a programme of any length, and the integrated value updates every 100 ms.

**Live meter.** `AudioEngine` measures its output while the transport plays. A seek or `resetLoudness()` restarts the measurement; pausing does not. The values reach the UI through relaxed atomics (`AudioEngine::loudness()`). Offline engines turn the meter off (`EngineConfig::meterLoudness`), because their callers measure for themselves.

**Validation** (`tests/dsp/LoudnessTests.cpp`):

- EBU Tech 3341 tests 1–5 (steady tones at -23 and -33 dBFS, absolute and relative gating), at 44.1 and 48 kHz, ±0.1 LU.
- EBU Tech 3342 tests 1–4 (loudness range 10, 5, 20 and 15 LU), ±1 LU.
- An inter-sample peak at fs/4 (sample peak -3.01 dBFS, true peak 0 dBTP).
- Mono reads 3.01 dB below the same signal on both stereo channels.
- Results are identical for block sizes from 1 to 2^20 samples.

## 2. Stem report (`session/StemReport`)

`analyzeStems(model, pool)` streams the project in 4,096-frame blocks. Memory stays small for any song length, and the report takes about 2.5 s for a 6-minute song with 9 stems (Release build, stems measured in parallel).

- **One private engine per track** renders the stem exactly as it plays: fader and pan applied, mute and solo ignored (muted stems are still reported), master at 0 dB. One more engine renders the mix as heard, including mute, solo and master.
- **Per stem:**
  - BS.1770 loudness and sample peak.
  - Energy share of all stems.
  - 50 ms RMS frames, used for the noise floor, activity and digital silence.
  - Stereo correlation and side ratio.
  - DC offset.
  - Full-scale samples in the source media.
  - Hann-windowed FFT power, giving six bands, the share below 120 Hz and the spectral centroid.
- **Noise floor:** the 10th percentile of the non-silent RMS frames, reported only when it lies below -40 dBFS and the 95th percentile rises at least 20 dB above it. A stem that never rests has no measurable bed. A stem is *active* where its frames exceed `max(floor + 10 dB, -70 dBFS)`, or -60 dBFS when no floor was found.
- **Between stems:** the cross-spectrum `Re(Xa · conj(Xb))`, accumulated per band (below 120 Hz, 120 Hz–4 kHz, above 4 kHz, full range) and normalised by both stems' band power. This is the zero-lag correlation of the band-limited signals. It measures what two stems share *in phase*, which is exactly what separation bleed and doubled parts look like.

### 2.1 Advice rules

Thresholds are named constants at the top of `StemReport.cpp`. They were checked against a real 9-stem Suno song, whose findings matched an earlier manual analysis.

| Code | Severity | Fires when |
|------|----------|------------|
| `mix-clipping` | warning | mix true peak > 0 dBTP or any mix sample beyond full scale |
| `mix-ceiling` | suggestion | mix true peak between -1 and 0 dBTP |
| `mix-loudness` | info | always, with integrated loudness and LRA |
| `silent` | suggestion | the stem has no signal (integrated ≤ -70 LUFS) |
| `residue` | suggestion | ≥ 15 LU below the loudest stem and full-range correlation ≥ 0.30 with another stem |
| `quiet` | info | ≥ 15 LU below the loudest stem without such a twin |
| `other-stem` | suggestion | a stem named "Other" (whole word) contributing < 10 % (Suno's catch-all stem) |
| `noise-bed` | suggestion / info | floor above -66 dBFS (suggestion above -60) and active < 95 %, and no expander running yet; fix: expander 10 dB above the bed |
| `source-clipping` | info | full-scale samples in the source file |
| `dc-offset` | suggestion | DC offset above 0.002 (about -54 dBFS); fix: DC removal |
| `out-of-phase` / `mono` | warning / info | stereo correlation below -0.2 / above 0.999 |
| `shared-low-end` | suggestion | correlation below 120 Hz ≥ 0.5 and both stems ≥ 15 % low end; the louder low end keeps it; fix: 120 Hz, 24 dB/octave high-pass on the others |
| `extra-low-end` | suggestion | a stem that is ≥ 50 % low end but < 5 % of the mix under a bigger low-end stem: a second kick or bass that is not phase-aligned, so the correlation rule misses it; fix: the same high-pass |
| `bleed` | info | correlation between 120 Hz and 4 kHz ≥ 0.35 |

Advice is sorted by severity (warnings first). Each item names the stem it concerns (`track`) and, where relevant, the other stem (`otherTrack`). The desktop app uses these to select the stem's row. Items a clean-up chain can solve carry `fixes` (section 3).

Stems are measured **after their clean-up chains**, so a problem a chain already solves leaves the advice. Two rules take the chain into account directly: `noise-bed` is not repeated while an expander runs (the measured bed is then the expanded one), and a stem whose chain high-passes at 90 Hz or above has given up its low end, so the low-end rules skip it. Once an expander runs, its threshold is the line between *active* and bed.

### 2.2 Where it shows up

- **Desktop app:** the *Stem Report* panel (bottom dock) runs on its own once imported or opened stems are loaded. Fader, pan, mute and master changes mark it *out of date* until **Analyze** (or Ctrl+R) runs it again. The report is also written to `Analysis/stem-report.json` in the project folder (format `stem-dawg-stem-report`, version 1).
- **Command line:** `stem-report <folder | files…> [--json] [--apply-fixes]` (`tools/stem-report`) prints the same report. `--apply-fixes` applies every fix and reports the stems again, after their chains.

## 3. Clean-up chain (`dsp/CleanupChain`)

Every track has a non-destructive chain, applied live before the fader and saved in the project (`tracks[].cleanup`, written only when it differs from the default):

1. **DC removal:** one-pole, one-zero high-pass at 5 Hz.
2. **High-pass:** Butterworth, 20 Hz–1 kHz, 12, 24 or 48 dB/octave (cascaded biquads with Butterworth pole Qs, exact -3 dB point).
3. **Downward expander:** the level is the RMS of both channels' mean power over 2 ms, the same measure the noise floor uses. Below the threshold the gain falls `ratio` times faster, by at most `range` dB, with a 6 dB soft knee. Attack 1 ms, hold 20 ms, release adjustable (default 80 ms).

**Lookahead without latency.** The expander measures the signal 5 ms before it is heard, so it is fully open when a note starts. The chain therefore delays its output by 5 ms. The engine hides that delay by reading each processed track 5 ms ahead of the playhead: stems are random access, so no latency compensation is needed. After a seek, a loop or a gap in rendering (a muted track), the chain restarts and is fed the 5 ms before the playhead first. The result is sample-identical to running the chain offline over the whole track, at any block size (tested).

**Engine.** Chain state lives in 256 preallocated slots claimed by track key when a snapshot is adopted. A track keeps its slot, and its filter memory, while its settings change. Tracks without a chain take the original mixing path, bit for bit. The engine publishes each chain's deepest gain reduction for the Clean-up panel's meter.

**Fixes.** The suggested expander threshold is the measured bed + 10 dB, with ratio `1 + range / 10` (1:3.4 at the default 24 dB range), so the bed itself drops by the full range. The margin was tuned on real Suno stems. With the 6 dB margin and 50 ms / 150 ms timing tried first, beds fell only 3–9 dB, because Suno beds are not steady and the gaps between notes are short. With the shipped settings, the beds of a 12-stem song fell 9–17 dB (Bass -41 to -55 dBFS, Synth -48 to -64, Other -58 to -75), while the integrated loudness of every expanded stem stayed within 0.1 LU.

**Desktop app.**
- The *Clean-up* panel (right dock, Ctrl+L) edits the chain of one track, with a gain-reduction meter and *Set from Noise Bed*.
- The C button on a track header opens it. It is blue when the chain is on and outlined when the chain is bypassed.
- *Apply Fix* and *Apply All Fixes* in the stem report set up the suggested chains.
- *Bypass Clean-up* (Ctrl+B) plays every track dry for A/B listening. It is not saved, and exports and the stem report always include the chains.

## 4. Tests

- `daw_dsp_tests`: the conformance cases above, plus allocation-free processing.
- `daw_engine_tests` `[loudness]`: the live meter reads -23 LUFS for the EBU tone, holds while paused, and restarts on seek and reset.
- `daw_dsp_tests` `[cleanup]`: exact lookahead delay; Butterworth response at the cutoff and one octave below for each slope; DC removal; the expander curve, knee continuity and monotonicity; a -50 dBFS noise bed drops by the full range while a note passes within 0.3 dB from its first millisecond; block-size invariance; stability while sweeping settings; no heap use.
- `daw_engine_tests` `[cleanup]`: a transparent chain plays bit-identical to the dry clip; engine output equals the chain run offline for block sizes 1 to 4096 (including blocks larger than the engine's maximum); after a seek; across a mid-play settings change (filter memory kept); mono media; slots following 12 reordered tracks that switch chains on and off, go silent and seek, with no heap use.
- `daw_project_tests` / `daw_integration_tests` `[cleanup]`: save and load, defaults for older files, clamping of hand-edited values; snapshot mapping and both bypasses; fixes on the noise-bed and shared-low-end advice, applying them, and a second analysis in which both problems are gone and the bass's loudness and activity are unchanged.
- `daw_integration_tests` `[stems]`: a synthetic stem set with known problems; every rule above is triggered and checked. The set has a bass with a -50.8 dBFS noise bed, percussion doubling the bass's low end, a mono synth, a residue copy of the synth 31 dB down, an empty stem and a clipping mix. The tests also check that mute, fader and master affect the right numbers, refusal of empty projects, and cancellation.
