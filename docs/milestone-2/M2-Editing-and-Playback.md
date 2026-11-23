# Stem DAWg — Milestone 2: Editing, undo and playback polish

The rest of Milestone 2: tracks can be renamed and deleted, every edit can be undone, playback no longer clicks when it stops or jumps, and long renders export as RF64.

## 1. Undo and redo (`session/UndoStack`)

**Snapshots, not inverse commands.** The project model is a small value type: audio lives in files and in the `MediaPool`, never in the model. Each undo step therefore stores the whole model as it was before the edit. Restoring a snapshot cannot drift out of sync with the edit, the way a hand-written inverse command can. Every new kind of edit is undoable without extra code. A 30-track project's model is a few kilobytes, and the history keeps 200 steps.

**One gesture, one step.** Edits carry a merge key:
- Edits with the same key that follow each other within a second merge into one step, keeping the state from before the first. A fader drag is one step, not 60.
- Grabbing the same fader again after a pause, or any other edit in between, starts a new step.
- A batch import merges for as long as its files take to load, so importing 15 stems undoes in one go.
- *Apply All Fixes* is a single step.

**What is undoable.**
- Import.
- Volume, pan, mute and solo.
- Master volume.
- Clean-up settings and stem report fixes.
- Rename and delete track.
- Artist profile and song details.

The playhead is not undoable: it is transport state. The *Bypass Clean-up* switch is not undoable either: it is a listening aid and is not saved.

**Analyses survive undo.** Analysis results describe media, not edits. Restoring a snapshot keeps every analysis record whose media is in the restored model, whichever state it came from. Undoing a track deletion therefore brings back its drum markers.

**Desktop app:**
- **Edit → Undo** (Ctrl+Z) and **Redo** (Ctrl+Y or Ctrl+Shift+Z). The menu names the step ("Undo Volume").
- The history is cleared when another project is opened or created.
- Text fields keep their own Ctrl+Z.

## 2. Renaming and deleting tracks

- **Rename:** double-click a track name, or right-click the header and choose *Rename Track…*. Clips named after the track follow the new name (`renameTrack`).
- **Delete:** right-click the header and choose *Delete Track*. Media that no other clip uses leaves the project, together with its analysis records (`removeTrack`). The files stay on disk, so undo can bring the track back. The status bar says so.

## 3. Declick (`AudioEngine`, `EngineConfig::declick`)

Stopping playback mid-waveform drops the output to zero in one sample, and seeking jumps between two unrelated waveforms. Both are audible clicks. With `declick` on, which the live device always turns on:

- **Stop** fades the output out over 5 ms, then stops. The playhead is put back where Stop was pressed, so playback resumes exactly there.
- **Seek** during playback fades out, jumps when the output is silent, then fades in at the new position. The audio after the fade-in is sample-exact.
- **Play** fades in from silence. Play during a stop's fade-out cancels the stop and fades back in.
- A stop or seek still fading out when the device stops is applied immediately.

Offline renders and the stem report leave `declick` off, so an export's first and last samples are exactly the mix. The fade is a linear ramp on the summed output, computed on the audio thread without allocation (tested).

## 4. RF64 export (`media/WavWriter`)

A WAV file's sizes are 32-bit, so a plain RIFF file cannot exceed 4 GB. That is about 4.1 hours of 24-bit stereo at 48 kHz, or 1.5 hours of 32-bit float stereo at 96 kHz. Above that size the writer produces **RF64** (EBU Tech 3306):
- `RF64` replaces `RIFF`.
- A `ds64` chunk after `WAVE` holds the 64-bit RIFF size, data size and frame count.
- The 32-bit size fields hold `0xFFFFFFFF`.

Smaller files are byte-for-byte unchanged. `WavContainer::Rf64` forces the format, and `WavContainer::Riff` refuses files that are too large. The tag reader follows `ds64`, so metadata after a huge data chunk is still found.

## 5. Tests

- `daw_integration_tests` `[undo]` / `[tracks]`:
  - undo and redo through several edits;
  - the redo history dropped by a new edit;
  - merging of a drag;
  - splitting after a pause, after another edit or after `seal()`;
  - an import batch that merges indefinitely;
  - the playhead and analyses kept across an undone delete;
  - the history limit;
  - rename (trimming, clips following, refusals);
  - delete (shared media kept until its last clip goes).
- `daw_engine_tests` `[declick]`:
  - fade-in on play, fade-out on stop with no step larger than one ramp increment;
  - the stop position kept;
  - a seek that is silent between the fades and sample-exact after them;
  - play cancelling a pending stop;
  - pending commands applied when the device stops;
  - no heap use.
- `daw_media_tests` `[rf64]`:
  - RF64 layout and `ds64` values;
  - identity with the RIFF file apart from the header and data size;
  - tags read back;
  - JUCE decoding the RF64 file;
  - the automatic switch at exactly 4 GB.
