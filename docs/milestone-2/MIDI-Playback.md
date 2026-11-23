# Stem DAWg — MIDI playback

Suno's advanced stem split exports a MIDI file next to each stem. Stem DAWg imports these (or any Standard MIDI File) as tracks, so the MIDI can be compared with the audio, layered on it, or used to rebuild a part with better sounds.

## How it works

- **A MIDI track is audio.** On load, the file is rendered offline through a SoundFont into an ordinary stereo `AudioAsset` at the project's sample rate (`media/Midi`). Everything downstream treats it like a stem: mixer, mute and solo, loudness metering, the stem report, export.
- **Parsing and synthesis:** TinyMidiLoader and TinySoundFont (MIT, single-header). The loader applies the file's tempo map; Suno's files carry hundreds of tempo events so the notes follow the song. Event times are resolved to 1 ms.
- **Import:** `importAudioFile` probes the MIDI file (length, average tempo, notes, drum use) and records it as stereo media at the project rate. The length is the last event plus a 2 s release tail. Loading renders it at that rate, so clip lengths stay exact.
- **Level:** renders start at -12 dB (`kMidiRenderGainDb`). General MIDI at full velocity is far louder than typical stems, and drum kits peak above full scale at 0 dB.
- **Tempo:** the first MIDI file imported into a project at the default tempo sets the project tempo to the file's average tempo. The bar.beat ruler then follows the music.
- **SoundFont sharing:** the 148 MB General MIDI set is loaded once and shared, using about 300 MB of memory as float samples. Every render uses its own synthesizer instance, so files render in parallel.
- **Changing the SoundFont** re-renders the project's MIDI tracks. MIDI peak caches are not kept, since peaks depend on the SoundFont.

## The FluidR3_GM download

FluidR3_GM (Frank Wen, MIT licence) is the default General MIDI set: 128 instruments plus drum kits. It is not stored in the repository or shipped with the app. Instead:

1. **Download.** On the first MIDI import (or from Settings → SoundFont for MIDI), the app downloads Debian's `fluid-soundfont-gm_3.1-5.3_all.deb` (114 MB) over HTTPS from deb.debian.org. If that fails, it uses Debian's permanent snapshot archive.
2. **Verify the package.** The package's SHA-256 is checked against the value pinned in `kFluidR3Gm` (`media/include/daw/media/Midi.h`).
3. **Unpack.** `extractDebMember` (`media/DebArchive`) streams the package through an ar reader, XZ Embedded and a tar reader, and writes `FluidR3_GM.sf2` to `%LOCALAPPDATA%\StemDAWg\SoundFonts` atomically. No external tools are needed.
4. **Verify the SoundFont.** The extracted file's SHA-256 is checked too; on any mismatch nothing is installed.

Any other SoundFont 2 file can be chosen instead.

## Tests

- `daw_media_tests` `[midi]`: a MIDI file and a one-sample SoundFont are built in code. The tests check note timing (onset within 1 ms of the MIDI time), pitch (Goertzel), release, tempo-map averaging, Unicode paths, bad files and cancellation.
- `daw_media_tests` `[deb]`: an embedded 700-byte package generated with Python's `tarfile` and `lzma`. Covers extraction by path suffix, GNU long names, damaged data and non-packages.
- `daw_integration_tests` `[midi]`: import → load → render through the session layer, including the missing-SoundFont error.
- `[real-soundfont]` (opt-in, hidden): installs the real Debian package and renders a real MIDI file. Set `STEMDAWG_TEST_DEB` and `STEMDAWG_TEST_MIDI` to run it.

## Limits

- Effects in the MIDI file (reverb and chorus sends) are not rendered; TinySoundFont plays the dry instruments.
- No piano-roll view yet: MIDI tracks show the waveform of their render.
- Very long or dense files take a few seconds to render on load.
