// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Track colours shared by the track headers and the waveform lanes. Stems get
// a colour from their name (vocals, drums, bass, ...) so a Suno stem set is
// readable at a glance; anything else cycles through the palette by position.

#include <QColor>
#include <QString>

#include <array>

inline QColor trackColor(const QString& name, int index)
{
    struct Rule {
        const char* keyword;
        QColor color;
    };
    static const std::array<Rule, 12> rules{{
        {"backing", QColor(0xb8, 0x3b, 0x8a)},
        {"vocal", QColor(0xd0, 0x2f, 0x78)},
        {"vox", QColor(0xd0, 0x2f, 0x78)},
        {"drum", QColor(0x2f, 0x7e, 0xd8)},
        {"perc", QColor(0x3f, 0x9a, 0x8c)},
        {"bass", QColor(0x6b, 0x7d, 0x99)},
        {"guitar", QColor(0xe0, 0x7b, 0x2c)},
        {"keyboard", QColor(0x2e, 0xa8, 0x5a)},
        {"keys", QColor(0x2e, 0xa8, 0x5a)},
        {"piano", QColor(0x2e, 0xa8, 0x5a)},
        {"synth", QColor(0x8e, 0x5c, 0xd6)},
        {"fx", QColor(0xc9, 0xa2, 0x27)},
    }};
    const QString lower = name.toLower();
    for (const Rule& rule : rules)
        if (lower.contains(QLatin1String(rule.keyword)))
            return rule.color;

    static const std::array<QColor, 8> palette{{
        QColor(0x2f, 0x7e, 0xd8), QColor(0xd0, 0x2f, 0x78), QColor(0x2e, 0xa8, 0x5a), QColor(0xe0, 0x7b, 0x2c),
        QColor(0x8e, 0x5c, 0xd6), QColor(0x3f, 0x9a, 0x8c), QColor(0xc9, 0xa2, 0x27), QColor(0x6b, 0x7d, 0x99),
    }};
    return palette[static_cast<std::size_t>(index) % palette.size()];
}
