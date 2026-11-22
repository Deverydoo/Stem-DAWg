// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Enables flush-to-zero / denormals-are-zero for the scope (x86-64 SSE).
// Denormal floats can cost 100x per operation in decaying filters/reverbs.

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>

namespace daw {

class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); } // FTZ | DAZ
    ~ScopedFlushDenormals() { _mm_setcsr(saved_); }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    unsigned int saved_;
};

} // namespace daw

#else

namespace daw {
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept = default;
};
} // namespace daw

#endif
