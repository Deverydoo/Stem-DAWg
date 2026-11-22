// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Settings > Artist Profiles: create, edit, duplicate and delete artist
// profiles. Each profile holds the metadata that is the same for every song by
// that artist and is merged into every export made under it.

#include <daw/project/Metadata.h>

#include <QDialog>

#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class ProfileDialog final : public QDialog {
    Q_OBJECT

public:
    ProfileDialog(daw::ProfileLibrary library, const std::string& selectId, QWidget* parent = nullptr);

    const daw::ProfileLibrary& library() const noexcept { return library_; }

private:
    void rebuildList(const std::string& selectId);
    void loadCurrent();
    void storeCurrent();
    void updateArtworkPreview();
    daw::ArtistProfile* current();

    daw::ProfileLibrary library_;
    bool loading_ = false;

    QListWidget* list_ = nullptr;
    QPushButton* duplicate_ = nullptr;
    QPushButton* remove_ = nullptr;
    QWidget* form_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* artist_ = nullptr;
    QLineEdit* albumArtist_ = nullptr;
    QLineEdit* composer_ = nullptr;
    QLineEdit* publisher_ = nullptr;
    QLineEdit* copyright_ = nullptr;
    QComboBox* genre_ = nullptr;
    QPlainTextEdit* comment_ = nullptr;
    QLineEdit* url_ = nullptr;
    QLineEdit* artwork_ = nullptr;
    QLabel* artworkPreview_ = nullptr;
};

/// Editable genre box pre-filled with suggestedGenres().
QComboBox* makeGenreBox(QWidget* parent);
