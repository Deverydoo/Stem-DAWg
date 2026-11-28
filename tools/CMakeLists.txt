# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# Command-line tools (no Qt).
add_executable(stem_report stem-report/main.cpp)
set_target_properties(stem_report PROPERTIES OUTPUT_NAME "stem-report" FOLDER "tools")
target_link_libraries(stem_report PRIVATE daw::session daw::compile_options)

add_executable(voice_denoise voice-denoise/main.cpp)
set_target_properties(voice_denoise PROPERTIES OUTPUT_NAME "voice-denoise" FOLDER "tools")
target_link_libraries(voice_denoise PRIVATE daw::ai daw::media daw::compile_options)
daw_copy_runtime_dlls(voice_denoise)

add_executable(drums_to_midi drums-to-midi/main.cpp)
set_target_properties(drums_to_midi PROPERTIES OUTPUT_NAME "drums-to-midi" FOLDER "tools")
target_link_libraries(drums_to_midi PRIVATE daw::session daw::compile_options)

add_executable(notes_to_midi notes-to-midi/main.cpp)
set_target_properties(notes_to_midi PROPERTIES OUTPUT_NAME "notes-to-midi" FOLDER "tools")
target_link_libraries(notes_to_midi PRIVATE daw::session daw::compile_options)
daw_copy_runtime_dlls(notes_to_midi)

add_executable(track_beats track-beats/main.cpp)
set_target_properties(track_beats PROPERTIES OUTPUT_NAME "track-beats" FOLDER "tools")
target_link_libraries(track_beats PRIVATE daw::session daw::compile_options)
