# Milestone 5: Mastering

M5 turns a cleaned-up mix into a release: loud enough for where it is going, never clipping, tonally balanced for its genre. This document covers what is done so far:

- **The master bus** with a **true-peak limiter**, and **loudness matching** to a target.
- **Tonal balance** against reference tracks you choose, with a **master EQ** and suggested moves.

Multiband compression and export presets follow (see [the roadmap](../ROADMAP.md#m5-mastering)).

## 1. The master bus, the true-peak limiter and loudness matching (`dsp/MasterChain`, `session/MasteringTools`)

### Why

Separated stems are not mixed to a level: they add up to whatever they add up to. The 9-stem Suno export sums to −12.3 LUFS with a true peak of **+5.0 dBTP** and 323 samples over full scale, so an Export Mix of it clipped unless the master fader was pulled down by 6 dB, which left it quieter than a streaming service plays it. Streaming services turn songs down to about −14 LUFS (Apple Music −16) and ask for a true peak of −1 dBTP or lower, so their lossy encoders do not clip; club and techno masters are usually much louder.

### The master bus

A new stage after the summed tracks, in the engine: tonal EQ (section 2), then the limiter. The master fader comes before it (the snapshot folds it into every track's gain, after the clean-up chains), so the fader is the drive into the limiter. It runs on everything that is heard and exported: live playback, Export Mix, and the stem report's measurement of the mix. Renders of single tracks (the report's per-stem measurements, noise learning) leave it out (`EngineConfig::masterChain`). It is off by default.

### The limiter

`TruePeakLimiter` keeps the true peak of its output (ITU-R BS.1770-4: 4x oversampled, the same interpolator as the loudness meter's true-peak reading) at or under the ceiling. Per input sample it computes the gain that brings the true peak of that stretch of signal to the ceiling, then:

1. takes the minimum of that gain over the lookahead window (5 ms, plus one sample);
2. lets it rise again with the release time (it falls at once);
3. averages it over the window;

and applies it to the audio delayed by the window. At every peak the averaged gain is at most what that peak needs, because the whole averaging window then lies inside the minimum's window; the gain fades into each peak linearly over 5 ms and out of it with the release. The minimum's window is one sample longer than the average's because a true peak lies between two original samples, and both must come down. Controls: **ceiling** (−12 to 0 dBTP, default −1) and **release** (10–1000 ms, default 100).

The delay (5.2 ms at 48 kHz) is the same whether the limiter is on or off, so switching it does not shift the audio. Export Mix renders that much longer and drops it from the start, so the file starts at frame 0 and its tail is complete; live playback simply hears the mix 5 ms later, and a seek clears the delay so nothing from before the seek plays.

Measured: on white noise 12 dB over, a quarter of the sample rate at 45° (every sample 3 dB under the true peak), a square wave, drum hits up to 24 dB over and single-sample clicks, the output's true peak never passes the ceiling by more than 0.02 dB (at ceilings of −6, −1 and −0.1 dBTP). A steady tone settles exactly at the ceiling. With 12 dB of gain reduction, a 50 Hz tone gets 0.14 % THD at 50 ms release, 0.08 % at 100 ms, 0.04 % at 200 ms (a 1 kHz tone under 0.01 % throughout), which is why 100 ms is the default.

### Loudness matching

**Match Loudness** (Master panel) finds the master fader setting that brings the mastered mix to the target integrated loudness through the limiter. Turning the fader up by N dB raises the loudness by less than N dB once the limiter works, so the answer is found by trying: the mix is rendered once (master fader at 0 dB, no master bus) into memory, then only the limiter and a loudness meter run over it at trial settings, refined by the secant method until the result is within 0.05 LU.

A peak limiter cannot make every mix every loudness: past a point it only crushes the music. The search never asks the limiter for more than **12 dB** of gain reduction; a target beyond that is reported as not reached, with the loudest setting within the limit. On the 9-stem export:

| Target | Master fader | Result | True peak | Loudness range | Deepest limiting | Trials |
|---|---|---|---|---|---|---|
| −16 LUFS (Apple Music) | −3.7 dB | −16.0 LUFS | −1.0 dBTP | 6.1 LU | 2.2 dB | 1 |
| −14 LUFS (streaming) | −1.7 dB | −14.0 LUFS | −1.0 dBTP | 6.1 LU | 4.2 dB | 1 |
| −10 LUFS (CD / Bandcamp) | +3.9 dB | −10.0 LUFS | −1.0 dBTP | 4.3 LU | 9.9 dB | 4 |
| −8 LUFS (club) | +6.0 dB | −9.5 LUFS, not reached | −1.0 dBTP | 3.5 LU | 11.9 dB | 7 |

Without the limit, the search went to +30 dB on the fader and 36 dB of limiting and still only reached −8.8 LUFS. Louder club masters need the multiband compression that comes later in M5. A match over a 6:21 song takes 3–13 s, most of it the render.

### In the app and the stem report

- **Master panel** (Master menu, Ctrl+M, tabbed with Clean-up): the limiter (ceiling, release, a gain-reduction meter), the loudness target (Streaming −14, Apple Music −16, CD / Bandcamp −10, Club −8 LUFS, or a custom value) and Match Loudness, which runs in the background, switches the limiter on and sets the master fader (undoable), then says what it did: "*The mix measures −12.3 LUFS (true peak 5.0 dBTP). With the master fader at −1.7 dB the master measures −14.0 LUFS, true peak −1.0 dBTP; the limiter turns the loudest peaks down by up to 4.2 dB.*"
- **Stem report:** the mix is measured as heard, master bus included. A mix that clips or passes −1 dBTP now gets "switch on the master limiter" as well as "lower the master fader"; the loudness note points to Match Loudness.
- **`stem-report --master[=LUFS]`** runs the match and reports the mastered mix.
- **Project file:** `mixer.master {limiter, ceilingDbtp, releaseMs, targetLufs}`, written only when it differs from the default.

### Tests

- `daw_dsp_tests` `[master]`: unity passthrough with the stated latency; the true peak at the ceiling on the five stress signals and three ceilings, and when driven 24 dB over; audio under the ceiling untouched; a steady tone at the ceiling and recovery after a burst; block-size independence; no allocation.
- `daw_engine_tests` `[master]`: the engine's master bus limits the mix and delays it by exactly `masterLatency()`; a seek clears the delay; without `masterChain` nothing changes; no allocation while playing and seeking.
- `daw_integration_tests` `[master]`: Export Mix with the limiter on but idle is sample-identical to the export without it; a clipping mix exports under the ceiling; Match Loudness reaches −14 LUFS within 0.05 LU under −1 dBTP without changing the project, and the export then measures it; a quiet target needs no limiting; an impossible target stops at 12 dB of limiting; a silent mix is refused.
- Serialization (written only when set, round trip, clamping) and the snapshot carrying the settings to the engine.

## 2. Tonal balance against reference tracks, and the master EQ (`dsp/TonalBalance`)

### Where the targets come from

A genre target (industrial, EBM, techno) has to come from measuring commercial releases in that genre. Published research gives the general shape of commercial music (a spectrum falling roughly 4.5–5 dB per octave over the midrange, with more low end in electronic music), but no per-genre curves that could be shipped, and inventing them would be guessing. So **targets are built from reference tracks the user owns**: pick a folder of releases you want to sound like, and each one is measured. The target is their average per band, and how much they disagree sets how close the mix must be. This also covers the roadmap's reference-track matching: a target from one track is a match to that track. Targets are saved in the app's data folder (`TonalTargets/*.json`) for every project, and the chosen one is copied into the project, so it compares the same way on any machine.

### The profile

The long-term spectrum in third-octave bands from 31.5 Hz to 16 kHz (8192-point frames, half overlapping), each band's share of the total energy in dB: only the shape counts, not the level. Frames more than 20 dB below the song's loud part (silence, fades, a long quiet intro) are left out.

### The comparison and the suggested EQ

The mix's profile minus the target's mean, per band, lined up on its weighted mean over 100 Hz–8 kHz (so a heavier sub-bass does not make every other band look thin). Bands further off than the references' spread are flagged.

A new **master EQ** sits before the limiter: five broad bands (low shelf 100 Hz, bells at 250 Hz, 1 kHz and 3 kHz, high shelf 8 kHz), for the tonal balance of the whole mix, not surgery. The suggestion is a weighted least-squares fit of its five gains (bands where the references agree count more) to the opposite of the difference. The fit goes through the same lining-up as the comparison, so its predicted result is what measuring the mix again reports; it is limited to ±6 dB and rounded to 0.5 dB.

### What did not work, and why

| Attempt | Result |
|---|---|
| Lining up on the median band, but fitting with the best mean offset | The two disagreed once the EQ moved several bands at once: the prediction said 1.6 dB, the remeasured mix 2.0 dB. |
| Holding the change in the EQ small (regularisation) rather than the EQ itself | Each Compare stopped short and the next one suggested half a dB more: the suggestion crept. Holding the total gains small makes it a fixed point: comparing again after applying it suggests the same EQ. |
| Checking the suggestion with `stem-report --apply-fixes` | That also applied the report's clean-up fixes (high-passes on two stems), which moved 100–160 Hz by 2 dB: the check now measures the EQ alone. |

### Measured

The 9-stem export against two sets of the user's own released songs (10 and 14 tracks; not genre references, but they show the mechanics):

| References | Main differences | Suggested EQ (low, low mids, mids, presence, air) | Deviation | Remeasured |
|---|---|---|---|---|
| Album A (10 tracks) | mids +2.9 dB (800 Hz +6.5), presence −2.3 dB | +1.5, +1.0, −3.5, +3.5, −1.0 dB | 2.4 → 1.6 dB predicted | 1.6 dB |
| Album B (14 tracks) | | −2.0, +4.0, −2.5, +1.5, −3.0 dB | 3.0 → 2.0 dB predicted | 2.0 dB |

What remains is narrow: an 800 Hz bump in the mix that a broad 1 kHz bell can only partly cancel, and sub-bass under 40 Hz that a 100 Hz shelf cannot separate from the low end.

### In the app

**Master panel → Tonal balance:** pick a target or build one with **New Target...** (a folder of reference tracks), then **Compare** measures the mix as heard (master EQ included). A chart shows the difference per band against the references' spread (orange outside it), a sentence says it in words ("*Against Album A (10 tracks): the mids 2.9 dB stronger, the presence 2.3 dB weaker. Suggested master EQ: ...*"), and **Apply Suggested EQ** sets the master EQ (undoable). The **EQ** group shows the five gains for editing by hand. `stem-report --target=<folder>` does the same from the command line.

### Tests

- `daw_dsp_tests` `[tonal]`: the shelf and bell sections have the gains they promise (no overshoot); the master EQ is transparent at 0 dB and moves the right bands; a profile puts a tone's energy in its band and leaves a quiet stretch out; a target averages its references and records their disagreement; a mix filtered by a known EQ gets each band of its inverse suggested within 1 dB, the remeasured result matches the prediction, and comparing again suggests the same EQ.
- `daw_integration_tests` `[tonal]`: a target from reference files skips an unreadable one and fails when none can be read; a target stored in a project compares the same way; the mix is profiled as heard, master EQ included.
- Serialization of the EQ and the target (an inconsistent target is dropped), and the snapshot carrying the EQ to the engine.
