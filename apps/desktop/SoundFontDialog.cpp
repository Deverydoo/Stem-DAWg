// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "SoundFontDialog.h"

#include "HelpText.h"

#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/core/Sha256.h>
#include <daw/media/DebArchive.h>
#include <daw/media/Midi.h>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
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

std::filesystem::path toPath(const QString& text)
{
    return daw::utf8ToPath(text.toStdString());
}

QString megabytes(std::uint64_t bytes)
{
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 0);
}

} // namespace

SoundFontDialog::SoundFontDialog(const QString& current, const QString& reason, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("SoundFont for MIDI"));
    setMinimumWidth(560);
    path_ = current;

    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText((reason.isEmpty() ? QString() : QStringLiteral("<p><b>%1</b></p>").arg(reason.toHtmlEscaped()))
                   + tr("<p>MIDI tracks are played through a <i>SoundFont</i>: a set of recorded instruments. "
                        "Stem DAWg can download <b>%1</b>, a complete General MIDI set with 128 instruments and drum kits "
                        "(%2 MB download, %3 MB installed).</p>"
                        "<p style='color:#9a9da5'>FluidR3_GM by Frank Wen, MIT licence. Downloaded from Debian's "
                        "fluid-soundfont-gm package and checked with SHA-256 before it is used.</p>")
                         .arg(QString::fromLatin1(daw::kFluidR3Gm.displayName), megabytes(daw::kFluidR3Gm.packageBytes),
                              megabytes(daw::kFluidR3Gm.fileBytes)));

    current_ = new QLabel(this);
    current_->setWordWrap(true);
    current_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setVisible(false);

    download_ = new QPushButton(tr("Download %1").arg(QString::fromLatin1(daw::kFluidR3Gm.displayName)), this);
    download_->setToolTip(helpTip(tr("Download FluidR3_GM"),
                                  tr("Downloads the General MIDI SoundFont from Debian, verifies it and installs it in\n%1")
                                      .arg(QDir::toNativeSeparators(defaultInstallPath()))));
    choose_ = new QPushButton(tr("Choose SF2 File..."), this);
    choose_->setToolTip(helpTip(tr("Use your own SoundFont"), tr("Any SoundFont 2 (.sf2) file works, for example a drum kit you like.")));
    close_ = new QPushButton(tr("Close"), this);
    connect(download_, &QPushButton::clicked, this, [this] { startDownload(0); });
    connect(choose_, &QPushButton::clicked, this, &SoundFontDialog::chooseFile);
    connect(close_, &QPushButton::clicked, this, &SoundFontDialog::reject);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(download_);
    buttons->addWidget(choose_);
    buttons->addStretch(1);
    buttons->addWidget(close_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(current_);
    layout->addWidget(progress_);
    layout->addWidget(status_);
    layout->addLayout(buttons);

    network_ = new QNetworkAccessManager(this);
    setPath(current);
}

SoundFontDialog::~SoundFontDialog()
{
    if (reply_)
        reply_->abort();
}

QString SoundFontDialog::defaultInstallPath()
{
    return toQString(daw::appDataDirectory() / "SoundFonts" / daw::kFluidR3Gm.fileName);
}

void SoundFontDialog::setPath(const QString& path)
{
    path_ = path;
    const bool usable = !path.isEmpty() && QFileInfo::exists(path);
    current_->setText(usable ? tr("In use: <b>%1</b>").arg(QDir::toNativeSeparators(path).toHtmlEscaped())
                             : tr("<span style='color:#e0a030'>No SoundFont is set up yet.</span>"));
    download_->setText(usable && QFileInfo(path).fileName() == QString::fromLatin1(daw::kFluidR3Gm.fileName)
                           ? tr("Download Again")
                           : tr("Download %1").arg(QString::fromLatin1(daw::kFluidR3Gm.displayName)));
}

void SoundFontDialog::setBusy(bool busy)
{
    download_->setEnabled(!busy);
    choose_->setEnabled(!busy);
    progress_->setVisible(busy);
    close_->setText(busy ? tr("Cancel") : tr("Close"));
}

void SoundFontDialog::reject()
{
    if (installing_)
        return; // extraction finishes on its own in a few seconds
    if (reply_) {
        reply_->abort();
        return;
    }
    QDialog::reject();
}

void SoundFontDialog::startDownload(int urlIndex)
{
    urlIndex_ = urlIndex;
    const char* url = urlIndex == 0 ? daw::kFluidR3Gm.url : daw::kFluidR3Gm.fallbackUrl;
    const QString target = QFileInfo(defaultInstallPath()).absolutePath() + QStringLiteral("/fluid-soundfont-gm.deb.part");
    QDir().mkpath(QFileInfo(target).absolutePath());
    file_.close();
    file_.setFileName(target);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        status_->setText(tr("Cannot write %1").arg(QDir::toNativeSeparators(target)));
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
        progress_->setValue(static_cast<int>(800.0 * static_cast<double>(received) / static_cast<double>(daw::kFluidR3Gm.packageBytes)));
        status_->setText(tr("Downloading... %1 of %2 MB")
                             .arg(megabytes(static_cast<std::uint64_t>(received)), megabytes(daw::kFluidR3Gm.packageBytes)));
    });
    connect(reply_, &QNetworkReply::finished, this, &SoundFontDialog::downloadFinished);
}

void SoundFontDialog::downloadFinished()
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
        daw::logging::warn("midi", "soundfont download failed",
                           {{"url", reply->url().toString().toStdString()}, {"error", reply->errorString().toStdString()}});
        QFile::remove(file_.fileName());
        if (urlIndex_ == 0) {
            startDownload(1); // Debian's permanent snapshot archive
            return;
        }
        setBusy(false);
        status_->setText(tr("Download failed: %1").arg(reply->errorString()));
        return;
    }
    installPackage(file_.fileName());
}

void SoundFontDialog::installPackage(const QString& package)
{
    installing_ = true;
    close_->setEnabled(false);
    status_->setText(tr("Verifying and unpacking..."));
    progress_->setValue(800);

    const std::filesystem::path deb = toPath(package);
    const std::filesystem::path target = toPath(defaultInstallPath());
    QPointer<SoundFontDialog> self(this);
    QThreadPool::globalInstance()->start([self, deb, target] {
        const auto report = [self](double fraction) {
            QMetaObject::invokeMethod(qApp, [self, fraction] {
                if (self)
                    self->progress_->setValue(800 + static_cast<int>(200.0 * fraction));
            });
        };
        QString error;
        auto packageHash = daw::sha256File(deb);
        if (!packageHash || *packageHash != daw::kFluidR3Gm.packageSha256) {
            error = QObject::tr("The download is damaged or not the expected file (checksum mismatch). Try again.");
        } else if (auto extracted = daw::extractDebMember(deb, daw::kFluidR3Gm.member, target, {}, report); !extracted) {
            error = QString::fromStdString(extracted.error().describe());
        } else if (auto fileHash = daw::sha256File(target); !fileHash || *fileHash != daw::kFluidR3Gm.fileSha256) {
            std::error_code ec;
            std::filesystem::remove(target, ec);
            error = QObject::tr("The unpacked SoundFont failed its checksum.");
        }
        std::error_code ec;
        std::filesystem::remove(deb, ec);
        QMetaObject::invokeMethod(qApp, [self, error, target] {
            if (!self)
                return;
            self->installing_ = false;
            self->close_->setEnabled(true);
            self->setBusy(false);
            if (!error.isEmpty()) {
                self->status_->setText(QObject::tr("Installation failed: %1").arg(error));
                return;
            }
            daw::logging::info("midi", "soundfont installed", {{"path", daw::pathToUtf8(target)}});
            self->setPath(toQString(target));
            self->status_->setText(QObject::tr("Installed and verified. MIDI tracks will use it."));
        });
    });
}

void SoundFontDialog::chooseFile()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose a SoundFont"), QFileInfo(path_).absolutePath(),
                                                      tr("SoundFont 2 (*.sf2)"));
    if (file.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto loaded = daw::loadSoundFont(toPath(file));
    QApplication::restoreOverrideCursor();
    if (!loaded) {
        QMessageBox::warning(this, tr("SoundFont"), QString::fromStdString(loaded.error().describe()));
        return;
    }
    setPath(file);
    status_->setText(tr("%n instrument(s) found.", nullptr, (*loaded)->presetCount()));
}
