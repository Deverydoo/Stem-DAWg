// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "ProfileDialog.h"

#include "HelpText.h"

#include <daw/core/Ids.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
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

} // namespace

QComboBox* makeGenreBox(QWidget* parent)
{
    auto* box = new QComboBox(parent);
    box->setEditable(true);
    box->setInsertPolicy(QComboBox::NoInsert);
    box->addItem(QString());
    for (const auto& genre : daw::suggestedGenres())
        box->addItem(q(genre));
    return box;
}

ProfileDialog::ProfileDialog(daw::ProfileLibrary library, const std::string& selectId, QWidget* parent)
    : QDialog(parent), library_(std::move(library))
{
    setWindowTitle(tr("Artist Profiles"));
    resize(760, 520);

    auto* intro = new QLabel(tr("A profile holds the details that stay the same for every song by one artist. "
                                "Pick the profile for a project in the toolbar; its details are written into every export. "
                                "Song fields (title, album, track number) are entered when exporting."),
                             this);
    intro->setWordWrap(true);

    list_ = new QListWidget(this);
    list_->setMinimumWidth(190);
    list_->setToolTip(helpTip(tr("Profiles"), tr("Your artist profiles. Select one to edit it.")));
    auto* add = new QPushButton(tr("New"), this);
    add->setToolTip(helpTip(tr("New profile"), tr("Create a profile for another artist or alias.")));
    duplicate_ = new QPushButton(tr("Duplicate"), this);
    duplicate_->setToolTip(helpTip(tr("Duplicate"), tr("Copy the selected profile as a starting point, e.g. for a side project on the same label.")));
    remove_ = new QPushButton(tr("Delete"), this);
    remove_->setToolTip(helpTip(tr("Delete profile"), tr("Remove the selected profile. Projects that used it keep their song fields.")));

    auto* listButtons = new QHBoxLayout;
    listButtons->addWidget(add);
    listButtons->addWidget(duplicate_);
    listButtons->addWidget(remove_);
    auto* left = new QVBoxLayout;
    left->addWidget(list_, 1);
    left->addLayout(listButtons);

    form_ = new QWidget(this);
    auto* form = new QFormLayout(form_);
    auto addField = [&](const QString& label, QWidget* field, const QString& tip) {
        auto* name = new QLabel(label, form_);
        name->setToolTip(tip);
        field->setToolTip(tip);
        form->addRow(name, field);
    };
    name_ = new QLineEdit(form_);
    artist_ = new QLineEdit(form_);
    albumArtist_ = new QLineEdit(form_);
    composer_ = new QLineEdit(form_);
    publisher_ = new QLineEdit(form_);
    copyright_ = new QLineEdit(form_);
    genre_ = makeGenreBox(form_);
    comment_ = new QPlainTextEdit(form_);
    comment_->setMaximumHeight(70);
    url_ = new QLineEdit(form_);
    artwork_ = new QLineEdit(form_);
    artworkPreview_ = new QLabel(form_);
    artworkPreview_->setFixedSize(96, 96);
    artworkPreview_->setAlignment(Qt::AlignCenter);
    artworkPreview_->setStyleSheet(QStringLiteral("border: 1px solid #44474e; color: #777;"));

    addField(tr("Profile name"), name_, helpTip(tr("Profile name"), tr("How the profile is listed in the toolbar switcher. Not written to files.")));
    addField(tr("Artist"), artist_, helpTip(tr("Artist"), tr("The performing artist shown by players and stores (ID3 TPE1, RIFF IART).")));
    addField(tr("Album artist"), albumArtist_, helpTip(tr("Album artist"), tr("Groups tracks into albums in music libraries (ID3 TPE2). Leave empty to use Artist.")));
    addField(tr("Composer"), composer_, helpTip(tr("Composer"), tr("Songwriter or producer credit (ID3 TCOM).")));
    addField(tr("Label / publisher"), publisher_, helpTip(tr("Label / publisher"), tr("Record label or publisher (ID3 TPUB). Your own name if self-released.")));
    addField(tr("Copyright"), copyright_,
             helpTip(tr("Copyright"), tr("Copyright line (ID3 TCOP, RIFF ICOP). Placeholders: {year} {artist} {title} {album}.\nExample: © {year} {artist}")));
    addField(tr("Genre"), genre_, helpTip(tr("Genre"), tr("Default genre for this artist (ID3 TCON, RIFF IGNR). A song can override it at export.")));
    addField(tr("Comment"), comment_,
             helpTip(tr("Comment"), tr("Free text added to every export (ID3 COMM, RIFF ICMT), e.g. credits or a link. Placeholders work here too.")));
    addField(tr("Website"), url_, helpTip(tr("Website"), tr("Artist web page (ID3 WOAR), e.g. your Bandcamp or SoundCloud.")));

    auto* browse = new QPushButton(tr("Browse..."), form_);
    auto* clearArt = new QPushButton(tr("Clear"), form_);
    auto* artRow = new QHBoxLayout;
    artRow->addWidget(artwork_, 1);
    artRow->addWidget(browse);
    artRow->addWidget(clearArt);
    auto* artWidget = new QWidget(form_);
    auto* artLayout = new QVBoxLayout(artWidget);
    artLayout->setContentsMargins(0, 0, 0, 0);
    artLayout->addLayout(artRow);
    artLayout->addWidget(artworkPreview_);
    addField(tr("Artwork"), artWidget,
             helpTip(tr("Artwork"), tr("Cover image (PNG or JPEG) embedded in every export (ID3 APIC). A song can use its own image instead.\n"
                                       "Square, 1400-3000 px and under ~2 MB is what most stores expect; big files make every WAV bigger.")));

    auto* body = new QHBoxLayout;
    body->addLayout(left);
    body->addWidget(form_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        storeCurrent();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { loadCurrent(); });
    connect(add, &QPushButton::clicked, this, [this] {
        storeCurrent();
        auto profile = daw::makeProfile(tr("New Artist").toStdString());
        library_.profiles.push_back(profile);
        if (library_.activeProfileId.empty())
            library_.activeProfileId = profile.id;
        rebuildList(profile.id);
        name_->setFocus();
        name_->selectAll();
    });
    connect(duplicate_, &QPushButton::clicked, this, [this] {
        storeCurrent();
        if (const auto* source = current()) {
            daw::ArtistProfile copy = *source;
            copy.id = daw::generateUuid();
            copy.name += tr(" (copy)").toStdString();
            library_.profiles.push_back(copy);
            rebuildList(copy.id);
        }
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        const auto* profile = current();
        if (profile == nullptr)
            return;
        if (QMessageBox::question(this, tr("Delete profile"), tr("Delete the profile \"%1\"?").arg(q(profile->name))) != QMessageBox::Yes)
            return;
        const std::string id = profile->id;
        std::erase_if(library_.profiles, [&](const daw::ArtistProfile& p) { return p.id == id; });
        if (library_.activeProfileId == id)
            library_.activeProfileId = library_.profiles.empty() ? std::string() : library_.profiles.front().id;
        rebuildList(library_.activeProfileId);
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose Artwork"), QFileInfo(artwork_->text()).absolutePath(),
                                                          tr("Images (*.png *.jpg *.jpeg)"));
        if (!file.isEmpty()) {
            artwork_->setText(file);
            storeCurrent();
            updateArtworkPreview();
        }
    });
    connect(clearArt, &QPushButton::clicked, this, [this] {
        artwork_->clear();
        storeCurrent();
        updateArtworkPreview();
    });
    for (QLineEdit* edit : {name_, artist_, albumArtist_, composer_, publisher_, copyright_, url_, artwork_})
        connect(edit, &QLineEdit::textEdited, this, [this] { storeCurrent(); });
    connect(genre_, &QComboBox::currentTextChanged, this, [this] { storeCurrent(); });
    connect(comment_, &QPlainTextEdit::textChanged, this, [this] { storeCurrent(); });
    connect(artwork_, &QLineEdit::editingFinished, this, &ProfileDialog::updateArtworkPreview);

    rebuildList(selectId.empty() ? library_.activeProfileId : selectId);
}

daw::ArtistProfile* ProfileDialog::current()
{
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(library_.profiles.size()))
        return nullptr;
    return &library_.profiles[static_cast<std::size_t>(row)];
}

void ProfileDialog::rebuildList(const std::string& selectId)
{
    const QSignalBlocker block(list_);
    list_->clear();
    int select = 0;
    for (std::size_t i = 0; i < library_.profiles.size(); ++i) {
        list_->addItem(q(library_.profiles[i].name));
        if (library_.profiles[i].id == selectId)
            select = static_cast<int>(i);
    }
    list_->setCurrentRow(library_.profiles.empty() ? -1 : select);
    loadCurrent();
}

void ProfileDialog::loadCurrent()
{
    loading_ = true;
    const daw::ArtistProfile* profile = current();
    form_->setEnabled(profile != nullptr);
    duplicate_->setEnabled(profile != nullptr);
    remove_->setEnabled(profile != nullptr);
    const daw::MetadataFields empty;
    const daw::MetadataFields& f = profile != nullptr ? profile->fields : empty;
    name_->setText(profile != nullptr ? q(profile->name) : QString());
    artist_->setText(q(f.artist));
    albumArtist_->setText(q(f.albumArtist));
    composer_->setText(q(f.composer));
    publisher_->setText(q(f.publisher));
    copyright_->setText(q(f.copyright));
    genre_->setCurrentText(q(f.genre));
    comment_->setPlainText(q(f.comment));
    url_->setText(q(f.url));
    artwork_->setText(q(f.artworkPath));
    updateArtworkPreview();
    loading_ = false;
}

void ProfileDialog::storeCurrent()
{
    daw::ArtistProfile* profile = current();
    if (loading_ || profile == nullptr)
        return;
    profile->name = s(name_->text());
    if (profile->name.empty())
        profile->name = s(artist_->text());
    profile->fields.artist = s(artist_->text());
    profile->fields.albumArtist = s(albumArtist_->text());
    profile->fields.composer = s(composer_->text());
    profile->fields.publisher = s(publisher_->text());
    profile->fields.copyright = s(copyright_->text());
    profile->fields.genre = s(genre_->currentText());
    profile->fields.comment = comment_->toPlainText().trimmed().toStdString();
    profile->fields.url = s(url_->text());
    profile->fields.artworkPath = s(artwork_->text());
    if (auto* item = list_->currentItem())
        item->setText(q(profile->name));
}

void ProfileDialog::updateArtworkPreview()
{
    const QPixmap image(artwork_->text().trimmed());
    if (image.isNull()) {
        artworkPreview_->setPixmap({});
        artworkPreview_->setText(artwork_->text().trimmed().isEmpty() ? tr("no artwork") : tr("not found"));
    } else {
        artworkPreview_->setPixmap(image.scaled(artworkPreview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}
