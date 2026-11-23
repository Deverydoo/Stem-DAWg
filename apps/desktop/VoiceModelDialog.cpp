// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "VoiceModelDialog.h"

#include "HelpText.h"

#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/session/VoiceTools.h>

#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QThreadPool>
#include <QVBoxLayout>

namespace {

QString toQString(const std::filesystem::path& path)
{
    return QString::fromStdString(daw::pathToUtf8(path));
}

QString megabytes(std::uint64_t bytes)
{
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1);
}

} // namespace

VoiceModelDialog::VoiceModelDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Voice Denoise Model"));
    setMinimumWidth(560);

    const auto& pkg = daw::kDeepFilterNet3;
    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText(tr("<p><b>Voice denoise</b> uses <b>%1</b>, a neural network that separates speech from noise: "
                      "fans, traffic, keyboards, room tone, noise that changes from moment to moment. "
                      "It runs on your computer; nothing is uploaded (%2 MB download).</p>"
                      "<p>It is made for <b>speech</b>: voice-overs, podcasts, spoken takes. A sung or heavily "
                      "effected voice can be taken for noise; for singing, use Noise reduction.</p>"
                      "<p style='color:#9a9da5'>DeepFilterNet3 by Hendrik Schröter, MIT or Apache-2.0 licence. "
                      "Downloaded from the project's GitHub repository (a fixed version) and checked with SHA-256 "
                      "before it is used.</p>")
                       .arg(QString::fromLatin1(pkg.displayName), megabytes(pkg.archiveBytes)));

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setVisible(false);

    download_ = new QPushButton(tr("Download %1").arg(QString::fromLatin1(pkg.displayName)), this);
    download_->setToolTip(helpTip(tr("Download the voice model"),
                                  tr("Downloads the model, verifies it and installs it in\n%1")
                                      .arg(QDir::toNativeSeparators(toQString(daw::voiceModelDirectory())))));
    close_ = new QPushButton(tr("Close"), this);
    connect(download_, &QPushButton::clicked, this, [this] { startDownload(0); });
    connect(close_, &QPushButton::clicked, this, &VoiceModelDialog::reject);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(download_);
    buttons->addStretch(1);
    buttons->addWidget(close_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(progress_);
    layout->addWidget(status_);
    layout->addLayout(buttons);

    network_ = new QNetworkAccessManager(this);
    if (daw::isVoiceModelInstalled()) {
        status_->setText(tr("The model is installed."));
        download_->setText(tr("Download Again"));
    }
}

VoiceModelDialog::~VoiceModelDialog()
{
    if (reply_)
        reply_->abort();
}

void VoiceModelDialog::setBusy(bool busy)
{
    download_->setEnabled(!busy);
    progress_->setVisible(busy);
    close_->setText(busy ? tr("Cancel") : tr("Close"));
}

void VoiceModelDialog::reject()
{
    if (installing_)
        return; // verifying and unpacking takes a moment
    if (reply_) {
        reply_->abort();
        return;
    }
    QDialog::reject();
}

void VoiceModelDialog::startDownload(int urlIndex)
{
    urlIndex_ = urlIndex;
    const auto& pkg = daw::kDeepFilterNet3;
    const char* url = urlIndex == 0 ? pkg.url : pkg.fallbackUrl;
    const QString dir = toQString(daw::voiceModelDirectory());
    QDir().mkpath(dir);
    file_.close();
    file_.setFileName(dir + QStringLiteral("/DeepFilterNet3_onnx.tar.gz.part"));
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        status_->setText(tr("Cannot write %1").arg(QDir::toNativeSeparators(file_.fileName())));
        return;
    }
    setBusy(true);
    progress_->setValue(0);
    status_->setText(tr("Downloading from %1...").arg(QUrl(QString::fromLatin1(url)).host()));

    QNetworkRequest request{QUrl(QString::fromLatin1(url))};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("StemDAWg/" DAW_APP_VERSION));
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, [this] { file_.write(reply_->readAll()); });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        const auto total = daw::kDeepFilterNet3.archiveBytes;
        progress_->setValue(static_cast<int>(900.0 * static_cast<double>(received) / static_cast<double>(total)));
        status_->setText(tr("Downloading... %1 of %2 MB").arg(megabytes(static_cast<std::uint64_t>(received)), megabytes(total)));
    });
    connect(reply_, &QNetworkReply::finished, this, &VoiceModelDialog::downloadFinished);
}

void VoiceModelDialog::downloadFinished()
{
    QNetworkReply* reply = reply_;
    reply->deleteLater();
    reply_ = nullptr;
    if (reply->error() == QNetworkReply::NoError)
        file_.write(reply->readAll());
    file_.close();

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        QFile::remove(file_.fileName());
        setBusy(false);
        status_->setText(tr("Download cancelled."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        daw::logging::warn("ai", "voice model download failed",
                           {{"url", reply->url().toString().toStdString()}, {"error", reply->errorString().toStdString()}});
        QFile::remove(file_.fileName());
        if (urlIndex_ == 0) {
            startDownload(1);
            return;
        }
        setBusy(false);
        status_->setText(tr("Download failed: %1").arg(reply->errorString()));
        return;
    }
    install(file_.fileName());
}

void VoiceModelDialog::install(const QString& archive)
{
    installing_ = true;
    close_->setEnabled(false);
    status_->setText(tr("Verifying and unpacking..."));
    progress_->setValue(900);

    const std::filesystem::path file = daw::utf8ToPath(archive.toStdString());
    QPointer<VoiceModelDialog> self(this);
    QThreadPool::globalInstance()->start([self, file] {
        const daw::Status status = daw::installVoiceModel(file);
        std::error_code ec;
        std::filesystem::remove(file, ec);
        const QString error = status ? QString() : QString::fromStdString(status.error().describe());
        QMetaObject::invokeMethod(qApp, [self, error] {
            if (!self)
                return;
            self->installing_ = false;
            self->close_->setEnabled(true);
            self->setBusy(false);
            if (!error.isEmpty()) {
                self->status_->setText(QObject::tr("Installation failed: %1").arg(error));
                return;
            }
            daw::logging::info("ai", "voice model installed", {{"path", daw::pathToUtf8(daw::voiceModelDirectory())}});
            self->accept();
        });
    });
}
