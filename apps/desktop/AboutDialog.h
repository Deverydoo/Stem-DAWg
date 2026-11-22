// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Help > About Stem DAWg: logo, version, licence and source location (the
// "Appropriate Legal Notices" AGPL-3.0 section 5(d) asks interactive programs
// to display), and the third-party components with their licences.

#include <QDialog>

class AboutDialog final : public QDialog {
    Q_OBJECT

public:
    /// `aiStatus`: inference runtime/provider line shown under the components.
    explicit AboutDialog(const QString& aiStatus, QWidget* parent = nullptr);
};
