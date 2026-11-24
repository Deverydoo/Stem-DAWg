// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Horizontal gain-reduction bar that grows to the left from 0 dB. Used for
// the clean-up expander and the master limiter. Feed it the deepest
// reduction since the last update (the engine's "take" readings); it shows
// that, decaying by 1.5 dB per update.

#include <QPainter>
#include <QWidget>

#include <algorithm>
#include <cmath>

class ReductionMeter final : public QWidget {
public:
    explicit ReductionMeter(QWidget* parent, double rangeDb = 30.0) : QWidget(parent), rangeDb_(rangeDb) { setFixedHeight(16); }

    void setReduction(float db)
    {
        shown_ = std::min(db, std::min(0.0f, shown_ + 1.5f));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(0, 0, -1, -1);
        p.fillRect(r, QColor(0x22, 0x24, 0x28));
        const double fraction = std::clamp(-static_cast<double>(shown_) / rangeDb_, 0.0, 1.0);
        const int w = static_cast<int>(std::lround(fraction * r.width()));
        p.fillRect(QRect(r.right() - w, r.top(), w, r.height()), QColor(0x4a, 0x90, 0xd9));
        p.setPen(QColor(0x3a, 0x3d, 0x44));
        p.drawRect(r);
        p.setPen(QColor(0xc8, 0xc8, 0xc8));
        p.drawText(r.adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   shown_ < -0.05f ? QStringLiteral("%1 dB").arg(static_cast<double>(shown_), 0, 'f', 1) : QStringLiteral("0 dB"));
    }

private:
    double rangeDb_;
    float shown_ = 0.0f;
};
