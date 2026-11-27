# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors
#
# cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P EmbedFile.cmake
# Writes a C++ source defining `const unsigned char <SYMBOL>[]` and
# `const std::size_t <SYMBOL>Size` (in namespace daw) with the file's bytes.

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
"// Generated from ${INPUT} by cmake/EmbedFile.cmake. Do not edit.\n"
"#include <cstddef>\n"
"namespace daw {\n"
"extern const unsigned char ${SYMBOL}[];\n"
"extern const std::size_t ${SYMBOL}Size;\n"
"alignas(16) const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n"
"const std::size_t ${SYMBOL}Size = ${size};\n"
"} // namespace daw\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
