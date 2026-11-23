# Milestone 4: AI-artifact repair

Stem separators leave artifacts that a home recording never has. M4 repairs them. This document covers what is done so far:

- **Bleed suppression:** each stem keeps a little of every other stem (the hi-hat ghosting through the vocal, the kick inside the bass). Bleed suppression turns that down, judging every stem against all the others.
- **Transient restoration:** AI-generated drums rise to each hit slowly and sound soft. The stem report measures how slowly; a transient shaper in the clean-up chain makes the hits rise faster without making them louder.
- **Harshness and shimmer control:** narrow peaks that keep ringing above the sound around them (a partial at 2–4 kHz that outlasts the notes, steady generator tones near 16 kHz) are turned down while they ring, and nothing else is.

Stereo repair follows (see [the roadmap](../ROADMAP.md#m4-ai-artifact-repair)).

## 1. Bleed suppression (`dsp/BleedSuppressor`, `session/BleedTools`)

### What a separator leaves behind

A separator (Suno's included) works on the mix's short-time spectrum. In each time-frequency bin it decides how much of the mix belongs to each stem, so in that bin every stem is a scaled copy of the mix. Measured on a 9-stem Suno export, the main stems agree with the phase of the stems' sum to 0.90–0.94 (1 would be a pure scaled copy).

Its mistakes show up as small shares. A stem keeps a few percent of bins that clearly belong to another stem: drum hits inside the vocal, the kick inside the bass. You hear them when a stem is soloed or turned up, and they are the noise beds that the stem report measures between notes (−48 to −53 dBFS on that export's bass, synth, drums and "Other").

### Method: sharpening the separator's shares

We have every stem, so each stem's share of every bin can be measured directly:

    M_s = |Y_s| / Σ_j |Y_j|

Raising the shares to a power *p* > 1 and renormalising gives the gain for each stem:

    G_s = M_s^(p−1) / Σ_j M_j^p,   limited to [−reduction, 0] dB

- A bin a stem owns (M_s near 1) keeps a gain of 1.
- A bin two stems share equally also keeps a gain of 1: (½)^(p−1) / (2·(½)^p) = 1. Parts that stems genuinely play together are not thinned.
- A small share is turned down. With *p* = 2, a stem holding 3 % of a bin drops by 30 dB, which the reduction limit caps.
- *p* = 1 changes nothing, and a lone stem is never touched.

The amplitudes are summed over 3 frames × 3 bins before the shares are taken, so the gains do not flutter ("musical noise"). The transform is the same as the spectral denoiser's: 2048-point frames, a hop of 512 and a square-root Hann window, with both channels sharing one complex FFT and one gain per bin. Reconstruction is exact.

| Strength | *p* | Reduction limit |
|---|---|---|
| Light | 1.5 | 12 dB |
| Medium (default) | 2 | 18 dB |
| Strong | 3 | 24 dB |

### What did not work

Two better-known approaches were measured first. Both fail on separator output, for reasons worth recording:

- **Estimating a leakage ratio per band** (the microphone-bleed approach: bleed in B ≈ λ·|A|², then a Wiener gain). λ can only be measured when B is silent and A is playing. A lead vocal is almost never silent, so the estimate came out absurd ("percussion leaks into the vocal at +4.5 dB") and the synth lost 6.6 dB of its music. On synthetic stems with known ground truth, every estimator (low percentile, inactive frames, detrended regression) removed under 0.5 dB of bleed. A fixed ratio per band cannot follow bleed that changes from bin to bin.
- **Coherent cancellation** (least-squares projection of B onto A in each bin over short windows, like an echo canceller). Because the separator splits each bin of the mix, all stems share the mix's phase there: a stem's own sound and its bleed are equally coherent with the other stems. Cancellation removed the stems' own sound as readily as the bleed, and made every stem worse than the input.

A caveat applies to sharpening as well. Post-processing that sees only the stems cannot undo errors the separator made inside a bin it split. On synthetic stems separated with blurred masks, sharpening removed 0.3–3 dB of bleed. What it does remove is the separator's floor: the small shares a stem keeps everywhere, which is what real Suno stems show.

### Measured

On a 9-stem Suno export (6:21, 48 kHz), Medium, with the stem report run before and after:

| Stem | Noise bed between notes | Louder half of the stem |
|---|---|---|
| Lead vocals | −67.8 → −86.7 dBFS | −0.1 dB |
| Backing vocals | −80.7 → −98.2 | −0.5 |
| Drums | −53.2 → −65.0 | −0.1 |
| Bass | −48.1 → −59.4 | −0.3 |
| Guitar | −80.2 → −97.6 | −0.5 |
| Keyboard | −66.9 → −85.0 | −1.6 |
| Percussion | −73.7 → −91.1 | −1.3 |
| Synth | −48.2 → −58.9 | −0.8 |
| Other | −49.3 → −60.0 | −1.2 |

The overlaps the stem report flags all fell (correlation across the full band):
- bass and synth, 0.42 → 0.33;
- lead and backing vocals, 0.42 → 0.34;
- drums and bass, 0.21 → 0.12;
- drums and percussion, 0.25 → 0.18.

Keyboard and synth stay strongly linked (0.39 → 0.36): on this song they really do play the same part.

**Speed:** all 9 stems of the song in 5.2 s, with the per-stem transforms spread over the CPU's cores.

### In the app

- **Clean-up panel > Bleed suppression (stems):** pick a strength, then click **This Track** or **All Stems**. **Clean-up > Suppress Bleed in All Stems** does the same at Medium. Applying to all stems is one undo step.
- **Non-destructive, but rendered,** like voice denoise. Every track of recorded audio takes part as a reference, from its *original* recording, aligned on the timeline. The tracks asked for get a processed copy in `Media/Processed` (derived from the original, `"process": "bleed-suppress"`, with the reduction limit as the amount), and their clips play it. **Original** switches back, and Ctrl+Z undoes it. Re-applying renders from the originals again. A track plays one rendered process at a time: applying bleed suppression replaces a voice denoise and vice versa.
- **Only what the clips play** is processed. Media outside the clips keeps the original.
- **A warning** appears when a stem's louder half drops by more than 3 dB. That stem mostly duplicates others (a second kick, a doubled part, residue), or the strength is too high.
- The stem report's overlap advice now points to bleed suppression.

### Disk space

Every apply writes new copies (about 1.3 GB for 9 stems of a 6-minute song in 32-bit float) and keeps the replaced ones for undo. When a saved project is closed, the copies that neither `project.json` nor the autosave uses are deleted (`ProjectStore::removeUnusedProcessedMedia`). Untitled projects are deleted as a whole, as before. Older backups of the manifest are not consulted (ten of them could pin over 10 GB). When a project opens with a copy missing, for example after restoring an older backup, its clips play the original instead, and the status bar says so.

### Command line

`stem-report <folder of stems> --bleed[=light|medium|strong] [--out=folder]` prints the stem report before and after. It also shows each stem's music and quiet-part change and the time taken. With `--out`, the copies are kept in `<folder>/Media/Processed` to listen to.

### Tests

- `daw_dsp_tests` `[bleed]`:
  - *p* = 1 reconstructs every stem exactly, one frame late;
  - a lone stem, and two stems that split every bin evenly, pass unchanged;
  - bleed is cut by exactly the reduction limit at each strength while the stem's own tone stays within 0.1 dB;
  - the result does not depend on the block size;
  - stereo channels stay apart;
  - no allocation while processing.
- `daw_integration_tests` `[bleed]`:
  - a stem placed 0.5 s into the timeline carries drum bursts that line up with the drum stem only on the timeline, and they are cut by the limit while its own tone stays;
  - re-applying renders from the original;
  - media outside the clip stays bit-identical, and a mono stem stays mono;
  - refusals: no other stem, a track that is not in the project, audio still loading.
- `daw_project_tests` `[store]`: unused copies are deleted on close, those that the manifest or autosave use are kept, and nothing is deleted when the manifest cannot be read.
- `daw_integration_tests` `[voice]`: clips whose copy is missing play their original again.

## 2. Transient restoration (`dsp/TransientShaper`, stem report "Attack")

### What is wrong with AI drums

Measured on the 9-stem Suno export, the drum stem's hits take about 5 ms to climb from −14 dB to their peak above 2 kHz, where a sampled electronic hit takes about 1 ms. The sum of all stems rises just as slowly, so the softness comes from the generation itself, not from the separation. There is no pre-echo before the hits to cut, either: the level climbs smoothly into each hit. The fix is to make the rise steeper.

### Measuring it: the stem report's Attack column

For every stem, the report follows the level of the mid signal above 2 kHz in 0.5 ms RMS frames. Below 2 kHz the envelope is the waveform itself: the first quarter-cycle of a 60 Hz kick alone takes 4 ms. A **hit** is a frame that is the highest within 10 ms before and 5 ms after it, and at least 15 dB above the lowest frame in the 15 ms before it. Its **rise time** runs from 10 % to 90 % of that jump, in amplitude, interpolated between frames. The top is the peak frame averaged with its two neighbours, because on a noisy hit (a snare, a hi-hat) a single frame is an extreme that the rise never really reaches.

The report keeps the loud hits (within 12 dB of the loudest) and shows their **median rise time** and **hits per minute**. It needs at least 20 hits. The measurement streams with the rest of the report and costs nothing noticeable.

On the Suno export, every stem rises in 1.3–3.6 ms; the lead vocal's consonants measure 2.0 ms. The attack time alone cannot tell drums from other parts, so the advice only goes to stems **named** for drums or percussion (drum, perc, kick, snare, hat, cymbal, toms, beat). Suno names its stems, so this works for the stems the app is built for.

**Advice:** a percussion stem whose hits take 2 ms or longer gets "*Drums: its hits take 2.7 ms to rise, where a sampled drum takes about 1 ms, so they sound soft and smeared. The transient shaper with +6 dB of attack brings the snap back.*", with the shaper as a one-click fix.

### The shaper

A new stage at the end of the clean-up chain, after the expander, live and non-destructive like the rest. It has two controls:

- **Attack** (−12 to +12 dB). Positive values make each hit rise faster; negative values soften hits that click.
- **Sustain** (−12 to +12 dB). This sets the gain on the decay after each hit: negative values tighten boomy drums, positive values bring tails up.

Turning a whole hit up does not sharpen it: the rise keeps its shape and the peak clips. The first version did exactly that, and the measured rise time barely moved (3.3 → 3.1 ms) while the drum stem's peak went from −2.9 to +2.5 dBFS. The attack control therefore reshapes the rise instead. It works on the band above 1 kHz, split off complementarily (high = x − low-pass(x)), so the sound is rebuilt bit-exactly at 0 dB. Fast gain changes below 1 kHz would only bend the waveform of a kick or a bass note. The shaper looks ahead by the chain's existing 5 ms lookahead:

- **F** is the band's RMS level over 0.5 ms, read for the middle of its window.
- **P** is the highest RMS level over 1.5 ms within the lookahead: the level the hit is heading for.
- **A** is F smoothed over 3 ms. F more than 1.5–3 dB above A means the level is climbing.

While the level climbs, the high band is lifted towards P: gain = min(attack, P/F − 0.5 dB). The slow start of a hit comes up, and the top, where F reaches P, is left alone. The lift is capped at P/F *after* smoothing, so a hit that already rises at once is not touched at all. Steady sound is never lifted either, because a level fluctuating above its recent average is also at or above the level ahead.

A negative attack turns the first stretch of each hit down, weighted by F against its 20 ms average. Sustain compares a 10 ms RMS of the whole signal with its peak, held with a 600 ms release. It acts once the sound falls 3 dB or more below that peak, reaches full effect 12 dB below, and is held off while a new hit rises. Each gain is smoothed over 0.5 ms.

### What did not work, and why

Each of these was measured on the real drum stem before being replaced:

| Attempt | Result |
|---|---|
| Boosting the attack portion (the classic transient designer) | The rise kept its shape; the peaks went up 5 dB and clipped. |
| A detector with a 0.5 ms attack | The detector's own lag looked like a rise: instant hits got +8 dB. |
| Single-sample peaks for P | On noise-like sound the next 5 ms always holds a higher sample, so every hit's top was lifted (+1–2 dB). |
| A held level (60 ms release) | In a busy drum part the level stays up from the last hi-hat, hiding the next hit's rise. |
| A 20 ms average for the rise | Held up by the hats and the previous hit; the first half of each rise stayed under it. |

### Measured

On the drum stem of the 9-stem Suno export (6:21), attack +6 dB, median over the 878 loud hits, level above 2 kHz relative to each hit's peak:

| Before the peak | Before | After |
|---|---|---|
| 3.0 ms | −9.5 dB | −6.3 dB |
| 2.5 ms | −7.0 dB | −4.1 dB |
| 2.0 ms | −5.4 dB | −3.2 dB |
| 1.0 ms | −3.8 dB | −2.6 dB |
| peak and after | | within 0.2 dB |

The stem report's attack time fell from 2.7 to 2.1 ms on Drums and from 3.6 to 2.9 ms on Percussion. Drums' sample peak moved by 0.1 dB. On synthetic hits that rise over 5 ms, the rise time falls by 30–50 %. The shaper costs about 0.1 % of one CPU core per track.

### Tests

- `daw_dsp_tests` `[transient]`:
  - at 0 dB the audio passes bit-exactly;
  - attack shortens a 5 ms rise by at least 30 % without raising the hit's level, the region before the hit or the decay;
  - negative attack lowers a hit's start by 3 dB or more;
  - an instant hit is left alone;
  - sustain shapes the tail by the set amount and leaves the hit;
  - steady 40 Hz, 100 Hz and 1 kHz tones are untouched;
  - the result does not depend on the block size;
  - switching off fades out without a jump;
  - no allocation.
- `daw_integration_tests` `[transient]`:
  - the stem report measures a 5 ms rise as 3–5 ms, an instant one under 1 ms, and 240 hits per minute;
  - only the soft drum stem gets the advice (not the sharp hi-hats, not a vocal);
  - applying the fix shortens the measured attack by at least 25 %.
- Serialization round trip and clamping, and the snapshot carrying the settings to the engine.

## 3. Harshness and shimmer control (`dsp/ResonanceSuppressor`, stem report "Harsh" / "Shimmer")

### What is harsh about AI songs

The long-term spectrum of a Suno song says little: a dB or two of bump around 2.5 kHz. The problem is narrow and shows up at finer resolution (2048-point frames, 23 Hz bins), measured on 24 Suno songs and the 9-stem export:

- **Partials that ring through many notes.** In one song a single partial at 1.86 kHz stands 11 dB above the spectrum around it, averaged over two minutes. Others ring at 2.2, 2.5 and 3.7 kHz. They sit on notes (A#6, C#7, D#7, A#7), so they come from the music, but they outlast the notes: tracked in half-second steps, a peak stands 12 dB proud one moment and below its surroundings the next. A static EQ cut would dull the song wherever the peak is not ringing.
- **Steady tones at fixed frequencies**: 8.0, 16.0, 16.6, 16.8 and 16.9 kHz, 6–11 dB above the high end around them, at the same frequencies in unrelated songs and in both 44.1 and 48 kHz files. They are generator artifacts, present in 63–71 % of the half-seconds of an affected song whatever the music does, and about 20 Hz wide.

Both are narrow peaks that **persist** above the spectrum around them. Ordinary partials move with the melody, and noise only pokes above its surroundings at random moments. That is what the detector looks for.

### The detector

It works on the frames of the clean-up chain's spectral stage (the same 2048-point short-time Fourier transform as noise reduction, so turning it on adds no delay). For every frame and bin above about 1.4 kHz:

- **Excess**: the bin's power over the higher of two neighbourhood averages, the bins 1/6 octave to its left and to its right, leaving out the two bins on each side of it (the peak's own skirt). A peak must stand above both sides, so a falling slope, or the cliff where a generator's low-pass cuts the high end off, does not count.
- **Persistence**: the excess in dB, smoothed over time, rising in about 1 s and falling in about 2 s. A partial of a note that comes and goes averages out; a resonance that outlasts the notes does not. Averaging in dB matters: a bin that goes dark pulls its average down hard.
- **Cut**: the smoothed excess above 2 dB, limited to the depth set for the range, so a peak is brought down towards its surroundings, never below. Each bin's cut also covers the bin on either side, because a partial between two bins, or one that drifts, spreads over its neighbours.

The two ranges are **harshness** (2–6 kHz, fading in over the half-octave below and out over the half-octave above) and **shimmer** (8–17.5 kHz, fading in over the half-octave below and out over a quarter-octave above). Between 6 and 8 kHz the fades overlap: sibilance territory gets part of either depth.

The engine resets every chain on a seek, and a detector that takes a second to settle would fade back in after every press of Play. Resonances last for the whole song, so the detector keeps its memory across resets; only preparing the chain clears it. A fresh detector averages over a few frames at first, so an export starts suppressing within a fraction of a second.

### In the stem report

Every stem runs through the same detector (one frame every 2048 samples is enough for statistics over seconds). For each range the report keeps the strongest peak: its frequency, the mean cut it would get at 6 dB of depth, the share of the song during which it is cut at all, and how high it stands above its surroundings while it is (`harshness` and `shimmer` in the JSON, *Harsh* and *Shimmer* columns in `stem-report`).

**Advice** (*harsh-resonance*, *shimmer-tones*) needs a mean cut of 2 dB or more, and the stem must supply at least 10 % of all stems' energy in the band where the peak sits. On the 9-stem export the bass has a 2 kHz peak that would qualify, but the bass supplies 4 % of the mix's energy there, so the other stems mask it. The flagged songs measure 2.1–3.7 dB (harshness, 5 of 24) and 2.0–3.9 dB (shimmer, 3 of 24); the others measure up to 1.9 dB, and a synthetic melody of bright 1 s notes reaches 1.8 dB. The margin is narrow: two songs sit at 1.9 dB, one of them on a 16.9 kHz generator tone, and the controls help there too even though the report stays quiet. For example: "*a resonance at 1.9 kHz rings about 10 dB above the sound around it during 86 % of the song, which sounds harsh*". The fix switches on the range with 6 dB of depth; a harshness fix and a shimmer fix for the same stem merge.

### What did not work, and why

| Attempt | Result |
|---|---|
| Smoothing over 30 ms (quick, like a compressor) | Every held note's bright partials were cut: a synthetic melody with 250 ms notes lost 3 dB at 2–6 kHz, white noise 0.4 dB. |
| The envelope as one power average around the bin | Bins just below a generator's 18.5 kHz low-pass cliff stood above it and were cut by 3–5 dB. |
| The envelope as an average in dB | The deep gaps between the partials of a bright note pull the average down, so every partial looked like a resonance. |
| A cut on the peak bin only | A partial between two bins kept half its energy in the neighbour: a 16 kHz line came down 2.8 dB instead of 5. |

### Measured

Four of the flagged songs, depth 6 dB in the flagged range, measured by the stem report before and after (height of the strongest peak above its surroundings while it rings):

| Song | Peak | Before | After |
|---|---|---|---|
| A | 16.0 kHz line | 6.9 dB | 3.2 dB |
| B | 16.6 kHz line | 6.0 dB | 2.9 dB (now the 16.0 kHz line) |
| C | 2.3 kHz resonance | 6.9 dB | 4.3 dB |
| D | 1.9 kHz resonance | 10.0 dB | 7.9 dB |

Steady peaks come down by most of the way; a resonance that rings for a few seconds at a time and wanders (song D) only by about 2 dB, because the detector, slow enough to leave melodies alone, catches only part of each episode. On synthetic signals a steady tone 14 dB above white noise comes down by the full 6 dB (±0.1 dB) and the noise around it does not change. The spectral stage costs about 0.4 % of one CPU core per track (the detection itself under 0.1 %); the stem report's measurement adds a fraction of a second.

### Tests

- `daw_dsp_tests` `[resonance]`:
  - steady white noise passes within 0.1 dB in both ranges;
  - a steady tone 14 dB proud at 3.7 kHz or 16 kHz comes down by the depth, the noise around it stays, and the other range's control does nothing to it;
  - a tone 6 dB proud comes down by less than the depth (towards its surroundings, never below), one under the threshold not at all;
  - tones at 800 Hz, 1.3 kHz and 21.5 kHz are never touched, and one at 7 kHz gets the taper's share of the depth;
  - a melody of 250 ms notes loses less than 0.5 dB;
  - noise with a 12 kHz low-pass cliff is not cut below the cliff;
  - the detector's memory survives reset and prepare clears it; depths are clamped; the chain restarts only when the spectral stage starts or stops;
  - no allocation.
- `daw_integration_tests` `[resonance]`:
  - the stem report finds a 3.7 kHz resonance and a 16 kHz tone, with advice and fixes for those stems only, not for plain noise, and not for a masked peak in a bass stem;
  - the two fixes merge on one track; the JSON lists the findings and the fixes;
  - applied, both peaks come down by more than 4 dB and the advice is gone.
- Serialization round trip and clamping, and the snapshot carrying the settings to the engine.

## Still to come in M4

- **Stereo repair:** mono low end, correlation-guided width.
