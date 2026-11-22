// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "MainWindow.h"

#include <daw/core/Files.h>
#include <daw/core/Logger.h>

#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>

#include <filesystem>
#include <system_error>

#include <shobjidl.h> // SetCurrentProcessExplicitAppUserModelID

namespace {

void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x22, 0x23, 0x27));
    p.setColor(QPalette::WindowText, QColor(0xd0, 0xd0, 0xd0));
    p.setColor(QPalette::Base, QColor(0x18, 0x19, 0x1c));
    p.setColor(QPalette::AlternateBase, QColor(0x22, 0x23, 0x27));
    p.setColor(QPalette::Text, Qt::white);                          // typed text in fields
    p.setColor(QPalette::PlaceholderText, QColor(0xa8, 0xab, 0xb3)); // grey hints ("from profile: ...")
    p.setColor(QPalette::Button, QColor(0x2c, 0x2e, 0x33));
    p.setColor(QPalette::ButtonText, QColor(0xd0, 0xd0, 0xd0));
    p.setColor(QPalette::Highlight, QColor(0xd9, 0x6a, 0x2a));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(0x2c, 0x2e, 0x33));
    p.setColor(QPalette::ToolTipText, QColor(0xd0, 0xd0, 0xd0));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x70, 0x70, 0x70));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x70, 0x70, 0x70));
    app.setPalette(p);
    app.setStyleSheet(QStringLiteral("QToolTip { color: #dcdcdc; background-color: #2c2e33; border: 1px solid #4a4d55; padding: 4px; }"));
}

QIcon applicationIcon()
{
    QIcon icon;
    for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256})
        icon.addFile(QStringLiteral(":/branding/stem-dawg-%1.png").arg(size), QSize(size, size));
    return icon;
}

/// The app was called "Industrial DAW" before it became Stem DAWg. Carry the
/// user's settings and artist profiles over once; the old data is left alone.
void migrateFromIndustrialDaw()
{
    QSettings current;
    if (current.value(QStringLiteral("migration/fromIndustrialDaw"), false).toBool())
        return;
    QSettings legacy(QStringLiteral("IndustrialDAW"), QStringLiteral("Industrial DAW"));
    for (const QString& key : legacy.allKeys())
        if (!current.contains(key))
            current.setValue(key, legacy.value(key));

    const std::filesystem::path profiles = daw::appDataDirectory() / "profiles.json";
    if (const wchar_t* base = _wgetenv(L"LOCALAPPDATA")) {
        const std::filesystem::path old = std::filesystem::path(base) / L"IndustrialDAW" / L"profiles.json";
        std::error_code ec;
        if (!std::filesystem::exists(profiles, ec) && std::filesystem::exists(old, ec))
            std::filesystem::copy_file(old, profiles, ec);
    }
    current.setValue(QStringLiteral("migration/fromIndustrialDaw"), true);
}

} // namespace

int main(int argc, char* argv[])
{
    // Own taskbar identity: groups windows and keeps pinned shortcuts on this icon.
    SetCurrentProcessExplicitAppUserModelID(L"ChaosRift.StemDAWg");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Stem DAWg"));
    QApplication::setApplicationDisplayName(QStringLiteral("Stem DAWg"));
    QApplication::setOrganizationName(QStringLiteral("StemDAWg"));
    QApplication::setApplicationVersion(QStringLiteral(DAW_APP_VERSION));
    QApplication::setWindowIcon(applicationIcon());
    applyDarkTheme(app);
    migrateFromIndustrialDaw();

    const auto logFile = daw::appDataDirectory() / "logs"
                         / daw::utf8ToPath("daw-" + daw::utcTimestampForFilename().substr(0, 8) + ".jsonl");
    (void)daw::Logger::instance().openFile(logFile);
    daw::logging::info("app", "starting", {{"version", DAW_APP_VERSION}, {"qt", qVersion()}, {"log", daw::pathToUtf8(logFile)}});

    int exitCode = 0;
    {
        MainWindow window;
        window.resize(1500, 900);
        window.show();
        const QStringList paths = QApplication::arguments().mid(1);
        if (!paths.isEmpty())
            window.openPaths(paths);
        exitCode = QApplication::exec();
    }

    daw::logging::info("app", "exiting", {{"code", exitCode}});
    daw::Logger::instance().flush();
    return exitCode;
}
