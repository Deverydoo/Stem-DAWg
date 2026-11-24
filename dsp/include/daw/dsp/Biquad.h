// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Second-order IIR section (transposed direct form II, double precision
// state). Real-time safe: no allocation, no locking.

#include <algorithm>
#include <cmath>
#include <complex>
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

    /// Peaking (bell) section: `gainDb` at `hz`, width set by q (RBJ cookbook).
    static BiquadCoefficients peaking(double hz, double q, double gainDb, double sampleRate) noexcept
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
        const double cosW = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha / a;
        BiquadCoefficients c;
        c.b0 = (1.0 + alpha * a) / a0;
        c.b1 = -2.0 * cosW / a0;
        c.b2 = (1.0 - alpha * a) / a0;
        c.a1 = c.b1;
        c.a2 = (1.0 - alpha / a) / a0;
        return c;
    }

    /// Low shelf: `gainDb` below `hz` (the midpoint of the transition); q = 1/sqrt(2) is the steepest without overshoot.
    static BiquadCoefficients lowShelf(double hz, double q, double gainDb, double sampleRate) noexcept
    {
        return shelf(hz, q, gainDb, sampleRate, false);
    }

    /// High shelf: `gainDb` above `hz`.
    static BiquadCoefficients highShelf(double hz, double q, double gainDb, double sampleRate) noexcept
    {
        return shelf(hz, q, gainDb, sampleRate, true);
    }

    /// Magnitude response at `hz`, in dB.
    double magnitudeDb(double hz, double sampleRate) const noexcept
    {
        const double w = 2.0 * std::numbers::pi * hz / sampleRate;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
        const std::complex<double> h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
        return 20.0 * std::log10(std::max(std::abs(h), 1e-12));
    }

private:
    static BiquadCoefficients shelf(double hz, double q, double gainDb, double sampleRate, bool high) noexcept
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
        const double cosW = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double root = 2.0 * std::sqrt(a) * alpha;
        const double sign = high ? -1.0 : 1.0; // the high shelf mirrors the cosine terms
        const double a0 = (a + 1.0) + sign * (a - 1.0) * cosW + root;
        BiquadCoefficients c;
        c.b0 = a * ((a + 1.0) - sign * (a - 1.0) * cosW + root) / a0;
        c.b1 = sign * 2.0 * a * ((a - 1.0) - sign * (a + 1.0) * cosW) / a0;
        c.b2 = a * ((a + 1.0) - sign * (a - 1.0) * cosW - root) / a0;
        c.a1 = -sign * 2.0 * ((a - 1.0) + sign * (a + 1.0) * cosW) / a0;
        c.a2 = ((a + 1.0) + sign * (a - 1.0) * cosW - root) / a0;
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
