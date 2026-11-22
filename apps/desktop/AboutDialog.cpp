// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "AboutDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

namespace {
constexpr const char* kRepositoryUrl = "https://github.com/Deverydoo/Stem-DAWg";
}

AboutDialog::AboutDialog(const QString& aiStatus, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("About Stem DAWg"));

    auto* logo = new QLabel(this);
    logo->setPixmap(QPixmap(QStringLiteral(":/branding/stem-dawg-logo-about.png")));
    logo->setAlignment(Qt::AlignCenter);

    const QString repo = QString::fromLatin1(kRepositoryUrl);
    auto* text = new QLabel(this);
    text->setTextFormat(Qt::RichText);
    text->setOpenExternalLinks(true);
    text->setWordWrap(true);
    text->setText(
        tr("<h3>Stem DAWg %1</h3>"
           "<p>A music cleaner and mastering tool for AI-generated stems and home recordings.</p>"
           "<p>Created by Craig &quot;Deverydoo&quot; Giannelli.<br>Copyright &copy; 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors.</p>"
           "<p>This program is free software: you can redistribute it and/or modify it under the terms of the "
           "<a href='https://www.gnu.org/licenses/agpl-3.0.html'>GNU Affero General Public License</a> as published by the "
           "Free Software Foundation, either version 3 of the License, or (at your option) any later version. "
           "It is distributed in the hope that it will be useful, but <b>WITHOUT ANY WARRANTY</b>; without even the implied "
           "warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.</p>"
           "<p>Source code: <a href='%2'>%2</a></p>"
           "<p><b>Built with</b><br>"
           "Qt %3 (LGPL-3.0) &middot; JUCE 8 (AGPL-3.0) &middot; ONNX Runtime (MIT) &middot; nlohmann/json (MIT)<br>"
           "See THIRD_PARTY_NOTICES.md in the source for details.</p>"
           "<p style='color:#9a9da5'>%4</p>")
            .arg(QStringLiteral(DAW_APP_VERSION), repo, QString::fromLatin1(qVersion()), aiStatus.toHtmlEscaped()));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(logo);
    layout->addWidget(text);
    layout->addWidget(buttons);
    setFixedWidth(620);
}
