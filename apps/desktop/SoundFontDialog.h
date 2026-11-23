// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <QDialog>
#include <QFile>
#include <QPointer>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QPushButton;

/// Chooses the SoundFont MIDI tracks play through: downloads, verifies and
/// installs FluidR3_GM, or uses any .sf2 file the user picks.
class SoundFontDialog final : public QDialog {
    Q_OBJECT

public:
    SoundFontDialog(const QString& current, const QString& reason, QWidget* parent = nullptr);
    ~SoundFontDialog() override;

    QString soundFontPath() const { return path_; }

    /// Where the downloaded FluidR3_GM is installed.
    static QString defaultInstallPath();

protected:
    void reject() override;

private:
    void startDownload(int urlIndex);
    void downloadFinished();
    void installPackage(const QString& package);
    void chooseFile();
    void setPath(const QString& path);
    void setBusy(bool busy);

    QString path_;
    QLabel* current_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* download_ = nullptr;
    QPushButton* choose_ = nullptr;
    QPushButton* close_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QFile file_;
    int urlIndex_ = 0;
    bool installing_ = false;
};
