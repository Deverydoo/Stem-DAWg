// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "ExportDialog.h"

#include "HelpText.h"
#include "ProfileDialog.h"

#include <daw/core/Files.h>

#include <QComboBox>
#include <QDate>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString q(const std::string& s)
{
    return QString::fromStdString(s);
}

std::string s(const QString& text)
{
    return text.trimmed().toStdString();
}

QString currentYear()
{
    return QString::number(QDate::currentDate().year());
}

/// Characters Windows does not allow in file names.
QString safeFileName(QString name)
{
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    for (QChar& c : name)
        if (bad.contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("Mix") : name;
}

QString describe(const daw::Destination& d, double projectRate)
{
    return QObject::tr("%1 LUFS, true peak under %2 dBTP, %3-bit %4 kHz")
        .arg(d.lufs, 0, 'f', 0)
        .arg(d.ceilingDbtp, 0, 'g', 2)
        .arg(d.format == daw::WavSampleFormat::Pcm16 ? 16 : 24)
        .arg(daw::destinationSampleRate(d, projectRate) / 1000.0, 0, 'g', 3);
}

} // namespace

ExportDialog::ExportDialog(Input input, std::function<std::optional<daw::ProfileLibrary>(const std::string&)> editProfiles,
                           QWidget* parent)
    : QDialog(parent), input_(std::move(input)), editProfiles_(std::move(editProfiles))
{
    setWindowTitle(tr("Export Mix"));
    resize(640, 680);

    // ---- profile
    auto* profileBox = new QGroupBox(tr("Artist profile"), this);
    profiles_ = new QComboBox(profileBox);
    profiles_->setToolTip(helpTip(tr("Artist profile"), tr("Whose metadata goes into the file: artist, label, copyright, website, artwork.")));
    auto* edit = new QPushButton(tr("Edit Profiles..."), profileBox);
    edit->setToolTip(helpTip(tr("Edit profiles"), tr("Add or change artist profiles.")));
    summary_ = new QLabel(profileBox);
    summary_->setWordWrap(true);
    summary_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    auto* profileRow = new QHBoxLayout;
    profileRow->addWidget(profiles_, 1);
    profileRow->addWidget(edit);
    auto* profileLayout = new QVBoxLayout(profileBox);
    profileLayout->addLayout(profileRow);
    profileLayout->addWidget(summary_);

    // ---- song
    auto* songBox = new QGroupBox(tr("Song (empty fields use the profile)"), this);
    auto* form = new QFormLayout(songBox);
    auto addField = [&](const QString& label, QWidget* field, const QString& tip) {
        auto* name = new QLabel(label, songBox);
        name->setToolTip(tip);
        field->setToolTip(tip);
        form->addRow(name, field);
    };
    title_ = new QLineEdit(q(input_.song.title), songBox);
    album_ = new QLineEdit(q(input_.song.album), songBox);
    track_ = new QLineEdit(q(input_.song.trackNumber), songBox);
    year_ = new QLineEdit(q(input_.song.year), songBox);
    genre_ = makeGenreBox(songBox);
    genre_->setCurrentText(q(input_.song.genre));
    isrc_ = new QLineEdit(q(input_.song.isrc), songBox);
    comment_ = new QLineEdit(q(input_.song.comment), songBox);
    artwork_ = new QLineEdit(q(input_.song.artworkPath), songBox);
    auto* browseArt = new QPushButton(tr("Browse..."), songBox);
    auto* artRow = new QHBoxLayout;
    artRow->addWidget(artwork_, 1);
    artRow->addWidget(browseArt);
    auto* artWidget = new QWidget(songBox);
    artWidget->setLayout(artRow);
    artRow->setContentsMargins(0, 0, 0, 0);

    addField(tr("Title"), title_, helpTip(tr("Title"), tr("Song title (ID3 TIT2, RIFF INAM). Defaults to the project name.")));
    addField(tr("Album"), album_, helpTip(tr("Album / release"), tr("Album, EP or single name (ID3 TALB, RIFF IPRD).")));
    addField(tr("Track number"), track_, helpTip(tr("Track number"), tr("Position on the release, e.g. 3 or 3/12 (ID3 TRCK).")));
    addField(tr("Year"), year_, helpTip(tr("Year"), tr("Release year (ID3 TYER, RIFF ICRD). Defaults to this year. Also used for {year} in the copyright.")));
    addField(tr("Genre"), genre_, helpTip(tr("Genre"), tr("Overrides the profile's genre for this song only.")));
    addField(tr("ISRC"), isrc_,
             helpTip(tr("ISRC"), tr("International Standard Recording Code, if your distributor assigned one (ID3 TSRC). Format: CC-XXX-YY-NNNNN.")));
    addField(tr("Comment"), comment_, helpTip(tr("Comment"), tr("Overrides the profile's comment for this song only.")));
    addField(tr("Artwork"), artWidget, helpTip(tr("Song artwork"), tr("Use a different cover for this song. Empty = the profile's artwork.")));

    // ---- file
    auto* fileBox = new QGroupBox(tr("File"), this);
    auto* fileForm = new QFormLayout(fileBox);
    destination_ = new QComboBox(fileBox);
    destination_->addItem(tr("As mixed (your master settings)"));
    for (const daw::Destination& d : daw::destinations())
        destination_->addItem(QStringLiteral("%1: %2 LUFS").arg(QString::fromUtf8(d.name)).arg(d.lufs, 0, 'f', 0));
    destination_->addItem(tr("All %n destinations (one file each)", nullptr, static_cast<int>(daw::destinations().size())));
    destination_->setCurrentIndex(std::clamp(input_.destinationIndex, 0, destination_->count() - 1));
    const QString destinationTip =
        helpTip(tr("Destination"),
                tr("As mixed: the file is your mix with the master exactly as you set it.\n"
                   "A destination masters a copy for where the song is going: Match Loudness for its target, its true-peak ceiling, "
                   "bit depth and sample rate. Your project is not changed.\n"
                   "Streaming services turn louder songs down to -14 LUFS, so a louder master gains nothing there; "
                   "Bandcamp, clubs and CDs do not, so those masters are louder."));
    auto* destinationLabel = new QLabel(tr("Destination"), fileBox);
    destinationLabel->setToolTip(destinationTip);
    destination_->setToolTip(destinationTip);
    fileForm->addRow(destinationLabel, destination_);
    destinationNote_ = new QLabel(fileBox);
    destinationNote_->setWordWrap(true);
    destinationNote_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    fileForm->addRow(QString(), destinationNote_);
    format_ = new QComboBox(fileBox);
    format_->addItem(tr("WAV 24-bit PCM (recommended: masters, distribution)"));
    format_->addItem(tr("WAV 16-bit PCM, dithered (CD, older players)"));
    format_->addItem(tr("WAV 32-bit float (no clipping; for further processing)"));
    format_->setCurrentIndex(std::clamp(input_.formatIndex, 0, 2));
    const QString formatTip = helpTip(tr("Format"), tr("24-bit is the standard for masters and what distributors (DistroKid, CD Baby...) prefer.\n"
                                                       "16-bit adds a tiny noise (dither) so quiet fades stay smooth.\n"
                                                       "32-bit float keeps peaks above 0 dBFS instead of clipping them."));
    auto* formatLabel = new QLabel(tr("Format"), fileBox);
    formatLabel->setToolTip(formatTip);
    format_->setToolTip(formatTip);
    fileForm->addRow(formatLabel, format_);
    path_ = new QLineEdit(fileBox);
    auto* browse = new QPushButton(tr("Browse..."), fileBox);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(path_, 1);
    pathRow->addWidget(browse);
    const QString pathTip = helpTip(tr("Save as"), tr("Where the WAV is written. An existing file with the same name is replaced."));
    auto* pathLabel = new QLabel(tr("Save as"), fileBox);
    pathLabel->setToolTip(pathTip);
    path_->setToolTip(pathTip);
    fileForm->addRow(pathLabel, pathRow);

    auto* note = new QLabel(tr("The whole song is rendered from the original stems with the current faders, pan, mute and solo, "
                               "at the project sample rate unless the destination needs another."),
                            this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #9a9da5;"));

    QLabel* mixNote = nullptr;
    if (!input_.mixNote.isEmpty()) {
        mixNote = new QLabel(input_.mixNote, this);
        mixNote->setWordWrap(true);
        mixNote->setStyleSheet(QStringLiteral("color: #e0a030;"));
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* exportButton = buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    exportButton->setDefault(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(profileBox);
    layout->addWidget(songBox);
    layout->addWidget(fileBox);
    layout->addWidget(note);
    if (mixNote != nullptr)
        layout->addWidget(mixNote);
    layout->addStretch(1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (path_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("Choose where to save the file."));
            return;
        }
        accept();
    });
    connect(profiles_, &QComboBox::currentIndexChanged, this, [this] {
        refreshInherited();
        suggestFileName();
    });
    connect(edit, &QPushButton::clicked, this, [this] {
        if (!editProfiles_)
            return;
        if (auto updated = editProfiles_(profileId())) {
            const std::string keep = profileId();
            input_.library = std::move(*updated);
            rebuildProfiles(input_.library.find(keep) != nullptr ? keep : input_.library.activeProfileId);
        }
    });
    for (QLineEdit* field : {title_, album_, track_, year_, isrc_, comment_, artwork_})
        connect(field, &QLineEdit::textChanged, this, [this] {
            refreshInherited();
            suggestFileName();
        });
    connect(genre_, &QComboBox::currentTextChanged, this, &ExportDialog::refreshInherited);
    connect(path_, &QLineEdit::textEdited, this, [this] { userEditedPath_ = true; });
    connect(path_, &QLineEdit::textChanged, this, &ExportDialog::refreshDestination);
    connect(destination_, &QComboBox::currentIndexChanged, this, [this] {
        suggestFileName();
        refreshDestination();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Export Mix"), path_->text(), tr("WAV audio (*.wav)"));
        if (!file.isEmpty()) {
            path_->setText(file.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive) ? file : file + QStringLiteral(".wav"));
            userEditedPath_ = true;
        }
    });
    connect(browseArt, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose Artwork"), QFileInfo(artwork_->text()).absolutePath(),
                                                          tr("Images (*.png *.jpg *.jpeg)"));
        if (!file.isEmpty())
            artwork_->setText(file);
    });

    rebuildProfiles(input_.profileId);
    suggestFileName();
    refreshDestination();
}

std::vector<const daw::Destination*> ExportDialog::destinations() const
{
    const auto all = daw::destinations();
    const int index = destination_->currentIndex();
    std::vector<const daw::Destination*> chosen;
    if (index >= 1 && index <= static_cast<int>(all.size()))
        chosen.push_back(&all[static_cast<std::size_t>(index - 1)]);
    else if (index == static_cast<int>(all.size()) + 1)
        for (const daw::Destination& d : all)
            chosen.push_back(&d);
    return chosen;
}

int ExportDialog::destinationIndex() const
{
    return destination_->currentIndex();
}

void ExportDialog::refreshDestination()
{
    const auto chosen = destinations();
    format_->setEnabled(chosen.empty());
    if (chosen.empty()) {
        destinationNote_->setText(tr("The master fader, limiter and loudness target as you set them."));
        return;
    }
    if (chosen.size() == 1) {
        destinationNote_->setText(tr("Mastered for %1 on a copy of the project: %2. Your master settings are not changed.")
                                      .arg(QString::fromUtf8(chosen.front()->key), describe(*chosen.front(), input_.projectRate)));
        return;
    }
    QStringList lines;
    lines << tr("One file each, mastered on a copy of the project:");
    const std::filesystem::path base = daw::utf8ToPath(path().toStdString());
    for (const daw::Destination* d : chosen)
        lines << QStringLiteral("%1  (%2)")
                     .arg(QString::fromStdString(daw::pathToUtf8(daw::destinationPath(base, *d).filename())), describe(*d, input_.projectRate));
    destinationNote_->setText(lines.join(QLatin1Char('\n')));
}

void ExportDialog::rebuildProfiles(const std::string& selectId)
{
    const QSignalBlocker block(profiles_);
    profiles_->clear();
    profiles_->addItem(tr("(no profile)"), QString());
    int select = 0;
    for (const auto& p : input_.library.profiles) {
        profiles_->addItem(q(p.name), q(p.id));
        if (p.id == selectId)
            select = profiles_->count() - 1;
    }
    profiles_->setCurrentIndex(select);
    refreshInherited();
}

const daw::ArtistProfile* ExportDialog::profile() const
{
    return input_.library.find(s(profiles_->currentData().toString()));
}

std::string ExportDialog::profileId() const
{
    return s(profiles_->currentData().toString());
}

daw::MetadataFields ExportDialog::song() const
{
    daw::MetadataFields f = input_.song; // keeps fields this dialog does not show
    f.title = s(title_->text());
    f.album = s(album_->text());
    f.trackNumber = s(track_->text());
    f.year = s(year_->text());
    f.genre = s(genre_->currentText());
    f.isrc = s(isrc_->text());
    f.comment = s(comment_->text());
    f.artworkPath = s(artwork_->text());
    return f;
}

daw::MetadataFields ExportDialog::resolved() const
{
    return daw::resolveMetadata(song(), profile(), s(input_.projectName), s(currentYear()));
}

void ExportDialog::refreshInherited()
{
    const daw::ArtistProfile* p = profile();
    const daw::MetadataFields inherited = daw::resolveMetadata({}, p, s(input_.projectName), s(currentYear()));
    title_->setPlaceholderText(q(inherited.title));
    year_->setPlaceholderText(q(inherited.year));
    genre_->lineEdit()->setPlaceholderText(inherited.genre.empty() ? QString() : tr("from profile: %1").arg(q(inherited.genre)));
    comment_->setPlaceholderText(inherited.comment.empty() ? QString() : tr("from profile: %1").arg(q(inherited.comment)));
    artwork_->setPlaceholderText(inherited.artworkPath.empty() ? tr("none") : tr("from profile: %1").arg(q(inherited.artworkPath)));

    const daw::MetadataFields r = resolved();
    QStringList lines;
    lines << tr("Artist: %1").arg(r.artist.empty() ? tr("(none)") : q(r.artist));
    if (!r.publisher.empty())
        lines << tr("Label: %1").arg(q(r.publisher));
    if (!r.copyright.empty())
        lines << tr("Copyright: %1").arg(q(r.copyright));
    if (!r.url.empty())
        lines << tr("Website: %1").arg(q(r.url));
    if (!r.artworkPath.empty())
        lines << tr("Artwork: %1").arg(QFileInfo(q(r.artworkPath)).fileName());
    if (p == nullptr)
        lines << tr("Tip: create a profile once (Settings > Artist Profiles) and every export gets your details.");
    summary_->setText(lines.join(QLatin1Char('\n')));
}

void ExportDialog::suggestFileName()
{
    if (userEditedPath_)
        return;
    const daw::MetadataFields r = resolved();
    QString base = q(r.title);
    if (!r.artist.empty())
        base = q(r.artist) + QStringLiteral(" - ") + base;
    const auto chosen = destinations();
    if (chosen.size() == 1)
        base += QStringLiteral(" (%1)").arg(QString::fromUtf8(chosen.front()->key));
    const QString dir = input_.suggestedDirectory.isEmpty() ? QDir::homePath() : input_.suggestedDirectory;
    path_->setText(QDir(dir).filePath(safeFileName(base) + QStringLiteral(".wav")));
}

daw::WavSampleFormat ExportDialog::format() const
{
    switch (format_->currentIndex()) {
    case 1:
        return daw::WavSampleFormat::Pcm16;
    case 2:
        return daw::WavSampleFormat::Float32;
    default:
        return daw::WavSampleFormat::Pcm24;
    }
}

int ExportDialog::formatIndex() const
{
    return format_->currentIndex();
}

QString ExportDialog::path() const
{
    return path_->text().trimmed();
}
