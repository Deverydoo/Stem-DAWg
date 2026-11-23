# Milestone 4: AI-artifact repair

Stem separators leave artifacts that a home recording never has. M4 repairs them. This document covers what is done so far:

- **Bleed suppression:** each stem keeps a little of every other stem (the hi-hat ghosting through the vocal, the kick inside the bass). Bleed suppression turns that down, judging every stem against all the others.

Transient restoration, harshness control and stereo repair follow (see [the roadmap](../ROADMAP.md#m4-ai-artifact-repair)).

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

## Still to come in M4

- **Transient restoration** for smeared drums.
- **Harshness and "shimmer" control:** dynamic EQ driven by spectral analysis.
- **Stereo repair:** mono low end, correlation-guided width.
