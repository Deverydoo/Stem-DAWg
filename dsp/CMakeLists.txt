# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# Signal processing building blocks shared by the engine (live metering), the
# analyzers and offline renders. Pure C++: no JUCE, no Qt.
daw_add_module(dsp
    SOURCES
        src/Fft.cpp
        src/TruePeak.cpp
        src/LoudnessMeter.cpp
        src/CleanupChain.cpp
        src/ResonanceSuppressor.cpp
        src/TransientShaper.cpp
        src/SpectralDenoiser.cpp
        src/Stereo.cpp
        src/DeepFilterDsp.cpp
        src/BleedSuppressor.cpp
    PUBLIC_DEPS
        daw::core)
