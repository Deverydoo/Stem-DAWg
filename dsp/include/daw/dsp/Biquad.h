// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Second-order IIR section (transposed direct form II, double precision
// state). Real-time safe: no allocation, no locking.

#include <cmath>
#include <numbers>

namespace daw {

/// Normalised coefficients (a0 == 1): y = b0 x + b1 x[-1] + b2 x[-2] - a1 y[-1] - a2 y[-2].
struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    /// High-pass section (RBJ cookbook form, exact cutoff after the bilinear
    /// transform). q = 1/sqrt(2) gives a 2nd-order Butterworth.
    static BiquadCoefficients highPass(double hz, double q, double sampleRate) noexcept
    {
        const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
        const double cosW = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        BiquadCoefficients c;
        c.b0 = (1.0 + cosW) / 2.0 / a0;
        c.b1 = -(1.0 + cosW) / a0;
        c.b2 = c.b0;
        c.a1 = -2.0 * cosW / a0;
        c.a2 = (1.0 - alpha) / a0;
        return c;
    }

    /// Low-pass section, same form.
    static BiquadCoefficients lowPass(double hz, double q, double sampleRate) noexcept
    {
        const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
        const double cosW = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        BiquadCoefficients c;
        c.b0 = (1.0 - cosW) / 2.0 / a0;
        c.b1 = (1.0 - cosW) / a0;
        c.b2 = c.b0;
        c.a1 = -2.0 * cosW / a0;
        c.a2 = (1.0 - alpha) / a0;
        return c;
    }
};

class Biquad {
public:
    void setCoefficients(const BiquadCoefficients& c) noexcept { c_ = c; }
    const BiquadCoefficients& coefficients() const noexcept { return c_; }
    void reset() noexcept { z1_ = z2_ = 0.0; }

    double process(double x) noexcept
    {
        const double y = c_.b0 * x + z1_;
        z1_ = c_.b1 * x - c_.a1 * y + z2_;
        z2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }

private:
    BiquadCoefficients c_;
    double z1_ = 0.0;
    double z2_ = 0.0;
};

} // namespace daw
