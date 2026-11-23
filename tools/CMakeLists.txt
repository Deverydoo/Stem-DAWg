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
