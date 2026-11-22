// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Hover help. Every control gets a short title and a plain-language
// explanation so the app is self-explanatory without a manual.

#include <QString>

inline QString helpTip(const QString& title, const QString& body)
{
    return QStringLiteral("<p style='white-space:pre-wrap'><b>%1</b><br>%2</p>").arg(title, body);
}
