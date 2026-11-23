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

/// Downloads, verifies and installs the DeepFilterNet3 voice model (8 MB).
/// accept()s once the model is installed.
class VoiceModelDialog final : public QDialog {
    Q_OBJECT

public:
    explicit VoiceModelDialog(QWidget* parent = nullptr);
    ~VoiceModelDialog() override;

protected:
    void reject() override;

private:
    void startDownload(int urlIndex);
    void downloadFinished();
    void install(const QString& archive);
    void setBusy(bool busy);

    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* download_ = nullptr;
    QPushButton* close_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QFile file_;
    int urlIndex_ = 0;
    bool installing_ = false;
};
