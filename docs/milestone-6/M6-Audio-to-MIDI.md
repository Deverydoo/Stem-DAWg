# Milestone 6: Audio to MIDI

M6 turns stems into MIDI you can edit, re-voice and build on. This document covers what is done so far:

- **Drums to MIDI:** a drum stem becomes a General MIDI drum file (kick, snare, hi-hats, crash) that lines up with the song.
- **Tempo and beat tracking** that follows an AI song's drifting tempo, with downbeats.
- **Exact MIDI timing:** our own MIDI file reader and writer, replacing a loader that drifted.

Pitched lines (bass, synths, keys) follow; see [the roadmap](../ROADMAP.md#m6-audio-to-midi).

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

Known limits: a kick half a beat after another, still booming, can read as a snare; toms are not transcribed yet (they come out as kick or snare); a crash that decays fast reads as an open hat.

## 3. Beats, bars and the MIDI file (`dsp/BeatTracker`, `session/DrumMidi`)

### Beat tracking

The beats are tracked on an envelope built from the hits, weighed by instrument (kick and snare 1, crash 0.5, hats 0.25): the beat is where kicks and snares fall. With every onset weighed alike, paired offbeat hats pulled the beats half a beat off for whole sections. The song's beat period comes from the envelope's autocorrelation, weighed towards 120 BPM against octave errors, and is refined per 8-second window (within 8 %). Dynamic programming (Ellis, "Beat tracking by dynamic programming", 2007) places the beats on strong hits while the spacing stays close to the local period; each beat then moves onto the nearest hit within 35 ms. Beats more than a beat outside the drums are dropped: through silence they would be guesses.

### Downbeats

Each beat's bar position is scored by its hits: kicks and crashes on the one, snares on the two and four. A Viterbi pass over the four phases picks the bar position beat by beat, a change of phase costing about three bars of evidence, so a beat gained or lost across a break, where there is nothing to track, does not shift the rest of the song's bars. Drums coming back after a break (two beats without kick, snare or crash) come in on a downbeat, which counts as much as a phase change. With a kick on every beat and few snares (this song has a snare in a quarter of its bars), the crashes and these re-entries are what remains to go on.

| On the GM render of Suno's drum MIDI | Beats (F, within 70 ms) | Downbeats on the true ones |
|---|---|---|
| With the side stick on most beats | 0.93 | 83 % |
| Side stick removed | 0.88 | 80 % |

The misses are the intro (toms only) and sparse breakdowns, where there is little to track, and a late section whose snare moves to the one and three.

### The MIDI file

General MIDI, channel 10: kick 36, snare 38, closed hat 42, open hat 46, crash 49. Each hit is placed by its beat (its position between the two beats around it) and quantised within the beat: to sixteenths, thirty-seconds, or not at all. The file changes tempo on every beat, so the notes stay on the audio although the song drifts, and a DAW's bar lines fall on the song's bars. Before the first beat, a lead-in of whole beats at its own tempo keeps time 0 at time 0; the beats before the first downbeat form a pickup bar with its own time signature, and a bar a beat short or long (where the bar phase changed) gets one too. Read back through its own tempo map, the file's notes land within 5 ms of the hits for 90 % of them (GM render, quantised to sixteenths).

## 4. Our own MIDI reader and writer (`media/MidiFile`)

`parseMidiFile` reads Standard MIDI Files (formats 0-2, metrical or SMPTE time, running status, note-on with velocity 0 as note-off) and computes every event's time in double precision through the tempo map. MIDI playback (`renderMidiFile`, `probeMidiFile`) now uses it, so an imported Suno MIDI file stays on its stems to the end. `encodeMidiFile` / `writeMidiFile` write type-1 files: a conductor track (name, tempo map, time signatures) and one track per part.

## 5. In the app

**Right-click a track name → Drums to MIDI** (quantise to 1/16 notes, to 1/32 notes, or as played), or **Analysis → Drums to MIDI** (the first track named like drums, or else the track the Clean-up panel shows). The track is rendered as it sounds (its clean-up and processed copies included) at unity gain, transcribed in the background, saved as `<track> (drums).mid` in the project's Exports folder (Documents/Stem DAWg/MIDI for an untitled project), and added as a MIDI track when a SoundFont is set, so it can be heard against the stem. The summary gives the notes per instrument, the tempo and the bars.

**`drums-to-midi <drums.wav> [--out=file.mid] [--grid=16]`** does the same from the command line, and scores a transcription: `--reference=ref.mid` (aligned per 10 s, or `--shift=ms`), or a MIDI input with `--soundfont=file.sf2`, which is rendered and transcribed back against itself.

## 6. Tests

- `daw_dsp_tests` `[drums]`: a synthesised kit (kick, snare, closed and open hats choked by the next, crash) plays a groove whose tempo drifts from 120 to 126 BPM: kick and hats F >= 0.95 and 0.9, snare >= 0.85, every crash found, hits within 15 ms, open hats told from closed (F >= 0.8); nothing in silence or on a held tone. Beats within 30 ms for 95 %, the tempo between 120 and 126 BPM, 90 % of the downbeats on the bars.
- `daw_media_tests` `[midifile]`: written files read back (notes, channels, velocities, tempo); 1,000 tempo changes stay exact to a microsecond; running status, velocity-0 note-offs, SMPTE time; broken and truncated files.
- `daw_integration_tests` `[drums]`: a drum track to MIDI: notes on the hits within 20 ms through the drifting tempo, a tempo change per beat that follows the drift, the project unchanged; quantisation within the beat, the lead-in and the pickup bar, velocities, as-played positions; a track without rhythm is refused.
