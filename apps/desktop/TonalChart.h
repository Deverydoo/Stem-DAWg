// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The mix's tonal balance against a target, per third-octave band: the grey
// band is how much the references disagree (the target's spread), the bars
// are how far the mix is off (orange where it is outside the spread).

#include <daw/dsp/TonalBalance.h>

#include <QPainter>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>

class TonalChart final : public QWidget {
public:
    explicit TonalChart(QWidget* parent) : QWidget(parent) { setMinimumHeight(120); }

    void setComparison(const daw::TonalComparison& comparison, const std::array<double, daw::kTonalBands>& spreadDb)
    {
        comparison_ = comparison;
        spread_ = spreadDb;
        update();
    }
    void clear()
    {
        comparison_.reset();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        constexpr double kRangeDb = 9.0;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QRect area = rect().adjusted(28, 6, -6, -18);
        p.fillRect(rect(), QColor(0x1c, 0x1e, 0x22));
        p.setPen(QColor(0x9a, 0x9d, 0xa5));
        if (!comparison_) {
            p.drawText(rect(), Qt::AlignCenter, tr("Pick a target and click Compare"));
            return;
        }
        const auto y = [&](double db) {
            return area.center().y() - static_cast<int>(std::lround(std::clamp(db, -kRangeDb, kRangeDb) / kRangeDb * area.height() / 2.0));
        };
        const double slot = static_cast<double>(area.width()) / daw::kTonalBands;
        for (std::size_t b = 0; b < daw::kTonalBands; ++b) {
            const int x0 = area.left() + static_cast<int>(b * slot) + 1;
            const int w = std::max(2, static_cast<int>(slot) - 2);
            p.fillRect(QRect(QPoint(x0, y(spread_[b])), QPoint(x0 + w, y(-spread_[b]))), QColor(0x33, 0x36, 0x3d));
            const double d = comparison_->deviationDb[b];
            const QColor bar = comparison_->outside[b] ? QColor(0xe0, 0x8a, 0x3c) : QColor(0x4a, 0x90, 0xd9);
            p.fillRect(QRect(QPoint(x0 + w / 4, y(0.0)), QPoint(x0 + w - w / 4, y(d))).normalized(), bar);
        }
        p.setPen(QColor(0x6a, 0x6d, 0x75));
        p.drawLine(area.left(), y(0.0), area.right(), y(0.0));
        p.setPen(QColor(0x9a, 0x9d, 0xa5));
        p.drawText(QRect(0, y(kRangeDb) - 6, 26, 12), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("+9"));
        p.drawText(QRect(0, y(-kRangeDb) - 6, 26, 12), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("-9"));
        const auto label = [&](double hz, const QString& text) {
            const auto it = std::min_element(daw::kTonalBandHz.begin(), daw::kTonalBandHz.end(),
                                             [&](double a, double b) { return std::abs(a - hz) < std::abs(b - hz); });
            const int x = area.left() + static_cast<int>((static_cast<double>(it - daw::kTonalBandHz.begin()) + 0.5) * slot);
            p.drawText(QRect(x - 20, area.bottom() + 2, 40, 14), Qt::AlignCenter, text);
        };
        label(100.0, QStringLiteral("100"));
        label(1000.0, QStringLiteral("1k"));
        label(10000.0, QStringLiteral("10k"));
    }

private:
    std::optional<daw::TonalComparison> comparison_;
    std::array<double, daw::kTonalBands> spread_{};
};
