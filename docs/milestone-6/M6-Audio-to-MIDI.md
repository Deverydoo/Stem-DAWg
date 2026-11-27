# Milestone 6: Audio to MIDI

M6 turns stems into MIDI you can edit, re-voice and build on. This document covers what is done so far:

- **Drums to MIDI:** a drum stem becomes a General MIDI drum file (kick, snare, hi-hats, crash) that lines up with the song.
- **Tempo and beat tracking** that follows an AI song's drifting tempo, with downbeats, kept in the project so the timeline's bars follow the song (section 6).
- **Notes to MIDI:** a pitched stem (bass, guitars, keys, synths, voices) becomes a MIDI file of its notes, with Spotify's Basic Pitch model, on the same beats as the drums (section 5).
- **Exact MIDI timing:** our own MIDI file reader and writer, replacing a loader that drifted.

Toms and the project's bar ruler follow; see [the roadmap](../ROADMAP.md#m6-audio-to-midi).

## 1. What we measured first

The work had a reference: Suno's advanced stem split exports a MIDI file per part, including a drum kit, next to the matching audio. Looking at it closely changed the plan in three places.

**AI songs drift in tempo.** Suno's drum MIDI carries a tempo change on every beat. Fitted per 32 beats, the song moves from 135.1 to 138.2 BPM and back (resetting at section changes). A fixed tempo grid would be hundreds of milliseconds off by the end, so the beats must be tracked, not assumed, and the MIDI file must carry a tempo map.

**Suno's MIDI is not ground truth.** Against its own drum audio it runs about 230 ms early for most of the song and about 17 ms early elsewhere, jumping by half a beat at section boundaries. It stamps key 37 (side stick) on 1,188 of its 3,363 notes: on most kicks, snares and sixteenth hats, as a second label for sounds already marked. About 300 of its hat notes have no onset in the audio near them, and its crash notes mostly do not ring like crashes. So the transcriber was developed against two references: Suno's MIDI rendered through the General MIDI SoundFont, whose hit times are exact by construction (with the side-stick notes removed, as they are not separate sounds in the song), and the real stem, scored against Suno's MIDI with the offset found per 10-second window.

**The MIDI loader drifted.** TinyMidiLoader, used for MIDI playback since 0.1, converts ticks to whole milliseconds and truncates the running time at every tempo change. A Suno export changes tempo on every beat: 764 changes lost 343 ms, so an imported Suno MIDI file ran a third of a second early by the end of the song. Section 4 replaces it.

## 2. Drum transcription (`dsp/DrumTranscriber`)

### Onset events

The stem (mono sum) is analysed in quarter-octave bands, 30 Hz to 16 kHz, from a 2048-point spectrum every 256 samples (43 ms window, 5.3 ms hop at 48 kHz), square-root compressed so the quiet top counts next to the loud low end. Each band's rise from one frame to the next is the onset signal. Summed with every band weighed by its typical rise (so a hat counts as much as a kick), its peaks are the events: at least 45 ms apart, above 1.5 times the local median, and with the bands rising well above where they were (a held tone's frame-to-frame wobble does not count). An event's rise per band is summed over the frames around its peak, less the band's typical rise.

A hit is timed at the frame of its peak plus 0.6 of the window, measured on SoundFont renders: matched hits come out within a millisecond of the truth (median), 90 % within 5 ms.

### Templates learned from the song

Each instrument's spectral template is learned from the song's own hits, not from a generic kit:

1. **Hat** first: the median shape of the top-heavy events that stop quickly (a short ring, below). In most grooves some hats play alone.
2. **Kick**: the shape the low-heavy events share (per band, the 30th percentile of each event normalised by its rise below 120 Hz), once each event's hat share is subtracted (measured above 8 kHz, where kicks put little). Without that step, a groove with hats on the eighths puts a hat on every kick, the hat becomes part of the kick, and hats on kicks vanish.
3. **Snare**: the shape the mid-heavy events keep once kick and hat are subtracted.
4. **Cymbal**: the median shape of clear crashes (below), once their kick is subtracted.

### All instruments at once

Each event's amount of each instrument is a non-negative least-squares fit of the four templates over six frequency regions (0-120 Hz, 120-300 Hz, 300 Hz-1 kHz, 1-3 kHz, 3-7 kHz, above 7 kHz), weighed by each region's typical rise. Fitting them together is what keeps a crash's mids from becoming a snare and a hat's mids from becoming one too. An instrument is hit when its amount reaches a share of its own loud hits (95th percentile): kick 0.3, snare 0.4, hat 0.2, cymbal 0.3. That ratio, against 1 for a loud hit, is the hit's strength, and its MIDI velocity (`10 + 100 * sqrt(strength)`: a loud hit plays at 110).

### Hats, open hats and crashes: the ring

The top band's power (above 6 kHz) after each hit tells them apart. A closed hat falls more than 7 dB within 40-100 ms of its peak; an open hat does not. A crash barely falls in its first 100 ms (less than 4 dB) nor much by 400 ms (less than 8 dB). A hat played while a crash rings also seems to ring, so a clear crash, the kind its template is learned from, must also raise the top band by 6 dB; a crash struck again while ringing (a crash on every beat) only has to ring. Only ringing events may hold a cymbal in the fit at all, or its broad shape takes the hats' share.

### Toms

The fit above has no tom template (toms come in several pitches), so it takes a tom for a kick, a snare or both. Each hit with some low end or body is measured again with finer spectra (4096 samples, about 85 ms: the band spectrogram's 30-60 Hz bands are mostly leakage): its tone (the lowest clear peak of what rises 40-800 Hz in the first 85 ms), how much of the tone is left 160-245 ms after the hit (its ring), its wires (2-8 kHz against 40-800 Hz) and how far the tone rose with the hit (its onset). Everything is measured against the song's own kicks and snares, because kits differ by 30 dB in how long a kick rings:

- **The song's kicks** (kick hits without a snare) set the ring to beat, their 25th percentile (which leaves out toms the fit took for kicks), and the kick's pitch.
- **The song's snare** is struck at one pitch, its toms at several: among the hits holding more of a loud snare than of a loud kick, the most common pitch is the snare's, and those hits set the wires to beat. A snare-like hit within half a semitone of that pitch is never a tom.
- **A tom** has fewer wires than the snares (2 dB less), keeps at least -45 dB of its tone, and either rings 12 dB longer than the kicks (at a pitch a whole tone or more from the kick's, else 20 dB longer) or rings at least as long as the kicks a major third or more above the kick's pitch, with 6 dB fewer wires.
- **The same drum struck again** before its ring is measured (a roll, a double kick, a tom played twice) sounds in that window at the same pitch: such a hit takes the ring of the run's last stroke, so a kick roll stays kicks.
- **A fill is judged by its first stroke**: toms within a ring of each other are one fill, and the fill counts only if its first stroke's tone rises at least 6 dB with the hit. A low note bleeding into the drum stem was already sounding; on the real stem this removed 92 false toms on hats over a sustained sub note. Within a fill, a stroke under a quarter of the fill's strongest is two toms beating, not a stroke.
- **Sorting**: the toms' pitches are split into three, two or one groups (exact 1-D k-means), the most groups for which each holds a tenth of the tom hits and neighbours are a whole tone apart, and written as floor, low and high tom (GM 41, 45, 48; two groups: floor and high; one: low).

Developed on six General MIDI kits (Standard, Room, Power, Electronic, TR-808, Jazz) playing grooves with tom fills, rendered through the SoundFont:

| Kit | Toms | Kick | Snare |
|---|---|---|---|
| Standard | 0.91 | 0.76 → 0.86 | 0.68 → 0.87 |
| Room | 0.91 | 0.89 → 0.90 | 0.72 → 0.90 |
| Jazz | 0.94 | 0.91 → 0.93 | 0.57 → 0.85 |
| TR-808 | 0.87 | 0.70 → 0.86 | 0.66 → 0.70 |
| Power | 0.30 | 0.77 → 0.84 | 0.64 → 0.71 |
| Electronic | 0.07 | 0.80 → 0.78 | 0.87 → 0.89 |

(kick and snare before → after toms; before, every tom was an extra kick or snare). The power kit's toms ring shorter than its kicks; the electronic kit's are noisy, with more wires than its snare: both are left as kicks and snares rather than risk the snares. On Suno's drum MIDI rendered through the SoundFont, all 18 toms are found (F 0.78), kicks unchanged (0.97); on the real stem, kicks unchanged (0.86) and 6 toms where Suno's MIDI has none. Sorting: on the acoustic kits the floor tom comes out as floor, the high tom as high; with four toms in the kit, the middle two share the low and high groups.

### What did not work

- **NMF on the whole spectrogram** (kick, snare, hat and free components, generic starting shapes): on uncompressed levels the loud low end dominates, and the factorisation split the spectrum by frequency instead of by instrument: its "kick" took 46-93 Hz, its "snare" the kick's upper body, its "hat" everything above 1 kHz.
- **Peeling one instrument at a time** (kick from the lows, then snare from the low mids, then hat from the top): a hi-hat's mids came out as snare, a crash entirely as snare.
- **A 21 ms window** for timing: worse kicks on the real stem (F 0.63 against 0.87), and the double detections it was meant to fix were an artifact of the drifting MIDI loader.

### Measured

| Reference | Kick | Snare | Hat | Crash |
|---|---|---|---|---|
| Suno's drum MIDI through the GM SoundFont, side stick removed (exact) | 0.97 | 0.84 | 0.92 | 0.75 |
| The same with the side stick (as its own snare-like sound) | 0.93 | 0.92 | 0.79 | 0.63 |
| The real Suno drum stem against Suno's MIDI | 0.86 | (0.38) | (0.47) | (0.04) |

F-measure within 50 ms. On the real stem only the kick score means much: Suno's snare, hat and crash labels are unreliable (section 1), and listening to the transcription against the stem is the better judge. A 6:21 song transcribes in about a second.

Known limits: a kick half a beat after another, still booming, can read as a snare; a crash that decays fast reads as an open hat; a kick struck with a tom comes out as the tom only; toms that ring shorter than the kicks, or as noisy as the snare, stay kicks and snares.

## 3. Beats, bars and the MIDI file (`dsp/BeatTracker`, `session/DrumMidi`)

### Beat tracking

The beats are tracked on an envelope built from the hits, weighed by instrument (kick and snare 1, crash 0.5, hats 0.25): the beat is where kicks and snares fall. With every onset weighed alike, paired offbeat hats pulled the beats half a beat off for whole sections. The song's beat period comes from the envelope's autocorrelation, weighed towards 120 BPM against octave errors, and is refined per 8-second window, within 4 % of it (these songs drift by about 2 %). A window with less than a quarter of the song's median onset energy (a sparse intro, a break) keeps the song's period: with nothing to lock on to, its autocorrelation used to land on the edge of the range, and the intro and a break of the test song were tracked at 150-156 BPM instead of 137. Dynamic programming (Ellis, "Beat tracking by dynamic programming", 2007) places the beats on strong hits while the spacing stays close to the local period; each beat then moves onto the nearest hit within 35 ms. Beats more than a beat outside the drums are dropped: through silence they would be guesses.

### Downbeats

Each beat's bar position is scored by its hits: kicks and crashes on the one, snares on the two and four. A Viterbi pass over the four phases picks the bar position beat by beat, a change of phase costing about three bars of evidence, so a beat gained or lost across a break, where there is nothing to track, does not shift the rest of the song's bars. Drums coming back after a break (two beats without kick, snare or crash) come in on a downbeat, which counts as much as a phase change. With a kick on every beat and few snares (this song has a snare in a quarter of its bars), the crashes and these re-entries are what remains to go on.

| On the GM render of Suno's drum MIDI | Beats (F, within 70 ms) | Downbeats on the true ones |
|---|---|---|
| With the side stick on most beats | 0.94 (0.93 with the local period within 8 %) | 83 % (83 %) |
| Side stick removed | 0.94 (0.88) | 86 % (80 %) |

On the real stem, the local tempo (median of 8 beat gaps) now stays between 132.4 and 138.9 BPM; it reached 154 before. The misses left are the intro (toms only), where there is little to track, and a late section whose snare moves to the one and three.

### The MIDI file

General MIDI, channel 10: kick 36, snare 38, closed hat 42, open hat 46, crash 49, floor tom 41, low tom 45, high tom 48. Each hit is placed by its beat (its position between the two beats around it) and quantised within the beat: to sixteenths, thirty-seconds, or not at all. The file changes tempo on every beat, so the notes stay on the audio although the song drifts, and a DAW's bar lines fall on the song's bars. Before the first beat, a lead-in of whole beats at its own tempo keeps time 0 at time 0; the beats before the first downbeat form a pickup bar with its own time signature, and a bar a beat short or long (where the bar phase changed) gets one too. Read back through its own tempo map, the file's notes land within 5 ms of the hits for 90 % of them (GM render, quantised to sixteenths).

## 4. Our own MIDI reader and writer (`media/MidiFile`)

`parseMidiFile` reads Standard MIDI Files (formats 0-2, metrical or SMPTE time, running status, note-on with velocity 0 as note-off) and computes every event's time in double precision through the tempo map. MIDI playback (`renderMidiFile`, `probeMidiFile`) now uses it, so an imported Suno MIDI file stays on its stems to the end. `encodeMidiFile` / `writeMidiFile` write type-1 files: a conductor track (name, tempo map, time signatures) and one track per part.

## 5. Notes to MIDI (`ai/NoteTranscriber`, `session/NoteMidi`)

### The model

[Basic Pitch](https://github.com/spotify/basic-pitch) (Bittner et al., "A Lightweight Instrument-Agnostic Model for Polyphonic Note Transcription and Multipitch Estimation", ICASSP 2022; Spotify, Apache-2.0) is a small convolutional network that hears any instrument, several notes at once. Its ONNX export is 230 KB, so it is compiled into the program (`third_party/basic-pitch`, embedded by `cmake/EmbedFile.cmake`): nothing to download, nothing to deploy next to the app, the tools or the tests.

It hears 2-second windows of 22.05 kHz mono audio and gives, per 11.6 ms frame and piano key, how likely a note sounds there and how likely one starts there. As in Basic Pitch's own inference, the audio is padded with 15 frames of silence, cut into windows 30 frames less than a window apart, and each window keeps its middle. Every kept frame's time is computed exactly from its window's position; the reference code instead corrects by a fixed amount per 172 frames, which drifts by up to 8 ms within each window. Eight windows go to the model at a time; a 6-minute stem takes about 6 seconds.

### Decoding notes

The decoding is Basic Pitch's (`output_to_notes_polyphonic`), ported line for line: onsets are also inferred from jumps in the note activation; each onset peak above a threshold starts a note on its key, latest first, which lasts until the activation stays under 0.3 for 11 frames; then the "melodia" pass repeatedly takes the strongest activation left and follows it both ways, adding notes that sound clearly without a clear start. The melodia pass's whole-matrix search for the maximum (once per note in the reference) is a max-heap here, which finds the same maximum because activations only ever drop to zero.

Checked against the reference pipeline (the Python code at commit fa5997a, run on the same stem through the same ONNX model): 1,446 of its 1,467 notes come out the same, key and start within 8 ms; the rest differ by the resampler and the reference's approximate frame times.

### What the defaults are, and why

Measured on Suno's MIDI parts rendered through the FluidR3_GM SoundFont (exact by construction), notes counted when the key matches and the start is within 50 ms, and a note doubled in octaves (Suno doubles its bass lines) counted once:

- **Shortest note 58 ms** instead of Basic Pitch's 127.7 ms. Suno's bass plays back-to-back sixteenths at 137 BPM, 109 ms each: with the default they were all merged or dropped (bass F 0.60 against 0.70).
- **Onset threshold by instrument**, from the track's name. Busy lines are found best from onsets of 0.4 (against Basic Pitch's 0.5: bass 0.75 against 0.70, synth keys 0.75 against 0.69, rhythm guitar 0.64 against 0.57; against 0.6: electric guitar 0.65 against 0.50, synth bass 0.57 against 0.40); sustained and sung parts from 0.6, where a lower threshold splits held notes (against 0.5: lead vocal 0.54 against 0.39, electric piano 0.82 against 0.80; against 0.4: backing vocals 0.51 against 0.28, pad 0.45 against 0.40). 0.6 applies to names with vocal, voice, vox, choir, pad, piano, string or organ, 0.4 to everything else.

| Part (Suno's MIDI through the SoundFont) | F | Real Suno stem against Suno's MIDI |
|---|---|---|
| Electric piano | 0.82 | 0.29 |
| Bass | 0.75 | 0.61 |
| Synth keys | 0.75 | 0.50 |
| Lead electric guitar | 0.70 | 0.44 |
| Electric guitar / rhythm guitar | 0.65 / 0.64 | 0.41 / 0.42 |
| Synth lead | 0.65 | 0.59 |
| Synth bass | 0.57 | 0.61 |
| Lead vocal / backing vocals | 0.54 / 0.51 | 0.29 / 0.22 |
| Synth pad | 0.45 | 0.23 |

The real stems are scored against Suno's MIDI aligned per 10-second window (it runs early by a varying amount, section 1), so that column says as much about Suno's MIDI as about the transcription. Where notes come out wrong, it is mostly octaves: a low note read an octave up, or a bright tone's upper partials read as notes two or three octaves above it. Notes are not dropped for sitting an octave above another, since octave doublings are real in these songs (Suno's bass parts are doubled throughout).

### The file

Each note becomes a note on its key, on channel 1, with the instrument chosen from the track's name (bass: Electric Bass, synth bass: Synth Bass, pad: Warm Pad, strings, electric piano, overdriven guitar for a lead guitar, choir for backing vocals...). Velocity is Basic Pitch's: 127 times the note's mean activation. When the project has a drum track (its name contains "drum", and it is not a MIDI track), the song's beats are tracked on it and the file uses the same tempo map as Drums to MIDI (`session/BeatTimeline`, shared by both), so the two files line up bar for bar. Without one, the file is a steady 120 BPM and the notes keep their played times. Notes can be quantised to sixteenths within the beat (start and end; a note never shrinks below a sixteenth); by default they are kept as played, since pitched parts are often played off the grid. One note is kept per key and start, and a note is shortened where the next one on its key starts.

## 6. In the app

**Analysis → Track the Beats** tracks the song's beats and bars on its drum track (named like a drum stem) and keeps them in the project (`timeline.tempo.beats`, in seconds, and `downbeats`, the bar lines among them; one undo step). The timeline's ruler then shows bars and beats along the song above the time, with faint bar lines across the tracks (and beat lines when zoomed in), and the position reads bar.beat.tick along the song (`project/MusicalTime`). Bars are numbered so the bar holding 0:00 is bar 1, as in a DAW; before the first tracked beat and after the last, beats continue at the neighbouring spacing. The tempo shown is the median, marked "tracked". Without tracked beats the bars run at the project's steady tempo from 0:00, and no bar lines are drawn across the tracks, since a steady grid on a drifting song would mislead. **Analysis → Steady Tempo** forgets the tracked beats. Drums to MIDI and Notes to MIDI use the project's beats when it has them, and keep the beats they track when it has none, so the timeline, the drum file and the note files all share one tempo map.

**Snapping.** With tracked beats, a selection's ends snap to the nearest beat, or, zoomed out, to the nearest bar line (every bar, every other bar, every fourth..., the finest grid whose lines are at least 12 pixels apart, counted from bar 1). Bars are found along the tracked beats, so a bar tracked a beat long or short still snaps to its own bar line. Hold Alt while dragging to select freely; **View → Snap to Beats** turns snapping off. The status bar gives a snapped selection's length in bars or beats ("Selected 23 beats (10.20 s) from 00:27.748 17.3.000"). A steady tempo is not snapped to, for the same reason it draws no bar lines.

**Right-click a track name → Drums to MIDI** (quantise to 1/16 notes, to 1/32 notes, or as played), or **Analysis → Drums to MIDI** (the first track named like drums, or else the track the Clean-up panel shows). The track is rendered as it sounds (its clean-up and processed copies included) at unity gain, transcribed in the background, saved as `<track> (drums).mid` in the project's Exports folder (Documents/Stem DAWg/MIDI for an untitled project), and added as a MIDI track when a SoundFont is set, so it can be heard against the stem. The summary gives the notes per instrument, the tempo and the bars.

**Right-click a track name → Notes to MIDI** (as played, or quantised to 1/16 notes), or **Analysis → Notes to MIDI** for the track the Clean-up panel shows: the track is rendered as it sounds, transcribed in the background, saved as `<track> (notes).mid` next to the drum file, and added as a MIDI track. The summary gives the notes, their range, the instrument, and whether the file follows the song's beats (and from which drum track).

**`drums-to-midi <drums.wav> [--out=file.mid] [--grid=16]`** does the same from the command line, and scores a transcription: `--reference=ref.mid` (aligned per 10 s, or `--shift=ms`), or a MIDI input with `--soundfont=file.sf2`, which is rendered and transcribed back against itself. **`notes-to-midi <stem.wav> [--out=file.mid] [--beats=drums.wav] [--grid=16]`** does the same for a pitched stem, with the same scoring (`--reference`, or a MIDI input with `--soundfont`; `--onset`, `--frame`, `--min-note` override the decoding).

## 7. Tests

- `daw_project_tests` `[tempo]`: bars and beats at a steady tempo and along tracked beats (a pickup, a tempo change, beats extrapolated before and after, a bar tracked a beat long, a time on a beat counting as that beat); snapping to the nearest beat, bar or every other bar, past the tracked beats and at a steady tempo; the median tempo from the beats; tracked beats saved, validated (increasing, bar lines in range) and absent from older projects.
- `daw_integration_tests` `[tempo]`: tracked beats kept in a project give the same MIDI timeline as the beats they came from.
- `daw_dsp_tests` `[toms]`: the synthesised kit with a tom fill every fourth bar (high, low and floor toms, sixteenths): toms F >= 0.85 within 15 ms, each tom mostly as its own kind, kicks and snares kept; no toms in a groove without them.
- `daw_dsp_tests` `[drums]`: a synthesised kit (kick, snare, closed and open hats choked by the next, crash) plays a groove whose tempo drifts from 120 to 126 BPM: kick and hats F >= 0.95 and 0.9, snare >= 0.85, every crash found, hits within 15 ms, open hats told from closed (F >= 0.8); nothing in silence or on a held tone. Beats within 30 ms for 95 %, the tempo between 120 and 126 BPM, 90 % of the downbeats on the bars.
- `daw_media_tests` `[midifile]`: written files read back (notes, channels, velocities, tempo); 1,000 tempo changes stay exact to a microsecond; running status, velocity-0 note-offs, SMPTE time; broken and truncated files.
- `daw_ai_tests` `[notes]`: the decoding on hand-made activations (a note from its onset to where the key stops sounding, a repeated note split at its second onset, a note without an onset found by the melodia pass, too-short notes and keys out of range dropped, onsets inferred from a jump); the model on a synthesised bass line with repeated sixteenths (F >= 0.85, starts within 10 ms median and 20 ms for 95 %) and on held chords over it (F >= 0.7); silence holds no notes.
- `daw_media_tests` `[midifile]`: a track's instrument written as a program change before its notes.
- `daw_integration_tests` `[notes]`: a bass track to MIDI on the song's beats (the notes, read back through the tempo map, on the audio within 30 ms for 90 %; Electric Bass; a tempo change per beat), quantised to sixteenths; a steady 120 BPM file without beats; a silent track refused; instruments and decoding from track names; one note per key and start, no note running into the next; the drum track found by name, not a MIDI track made from it.
- `daw_integration_tests` `[drums]`: a drum track to MIDI: notes on the hits within 20 ms through the drifting tempo, a tempo change per beat that follows the drift, the project unchanged; quantisation within the beat, the lead-in and the pickup bar, velocities, as-played positions; a track without rhythm is refused.
