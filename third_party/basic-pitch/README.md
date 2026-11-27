# Basic Pitch model

`nmp.onnx` is the ONNX export of Basic Pitch (Bittner et al., "A Lightweight
Instrument-Agnostic Model for Polyphonic Note Transcription and Multipitch
Estimation", ICASSP 2022), copyright 2022 Spotify AB, under the Apache License
2.0 (`LICENSE`).

Source: https://github.com/spotify/basic-pitch, file
`basic_pitch/saved_models/icassp_2022/nmp.onnx` at commit
`fa5997af0a8210982619003269994a1be25eddf3`, unmodified.
SHA-256 `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`.

It is compiled into `daw_ai` (see `ai/CMakeLists.txt`), so the app, the tools
and the tests need no model file next to them. `ai/NoteTranscriber` runs it.
