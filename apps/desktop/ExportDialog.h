// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// File > Export Mix: choose the artist profile, fill in song fields (empty
// fields inherit from the profile, shown as grey placeholder text), pick
// where the song is going (as mixed, or mastered for a destination: one or
// all of them) and the file.

#include <daw/media/WavWriter.h>
#include <daw/project/Metadata.h>
#include <daw/session/Destinations.h>

#include <QDialog>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;

class ExportDialog final : public QDialog {
    Q_OBJECT

public:
    struct Input {
        daw::ProfileLibrary library;
        std::string profileId;
        daw::MetadataFields song;
        QString projectName;
        QString suggestedDirectory;
        int formatIndex = 0;      // remembered between exports
        int destinationIndex = 0; // remembered between exports
        double projectRate = 48000.0;
        QString mixNote;     // e.g. "2 tracks muted": shown so nothing is left out by accident
    };

    /// `editProfiles` opens the profile editor and returns the updated
    /// library (or nullopt when cancelled).
    ExportDialog(Input input, std::function<std::optional<daw::ProfileLibrary>(const std::string&)> editProfiles,
                 QWidget* parent = nullptr);

    std::string profileId() const;
    daw::MetadataFields song() const;
    const daw::ProfileLibrary& library() const noexcept { return input_.library; }
    daw::WavSampleFormat format() const;
    int formatIndex() const;
    /// The path typed (for several destinations, the name their files are derived from).
    QString path() const;
    /// Destinations to master for: empty = as mixed.
    std::vector<const daw::Destination*> destinations() const;
    int destinationIndex() const;

    /// Song fields merged with the selected profile, as they will be written.
    daw::MetadataFields resolved() const;

private:
    void rebuildProfiles(const std::string& selectId);
    void refreshInherited();
    void suggestFileName();
    void refreshDestination();
    const daw::ArtistProfile* profile() const;

    Input input_;
    std::function<std::optional<daw::ProfileLibrary>(const std::string&)> editProfiles_;
    bool userEditedPath_ = false;

    QComboBox* profiles_ = nullptr;
    QLabel* summary_ = nullptr;
    QLineEdit* title_ = nullptr;
    QLineEdit* album_ = nullptr;
    QLineEdit* track_ = nullptr;
    QLineEdit* year_ = nullptr;
    QComboBox* genre_ = nullptr;
    QLineEdit* isrc_ = nullptr;
    QLineEdit* comment_ = nullptr;
    QLineEdit* artwork_ = nullptr;
    QComboBox* destination_ = nullptr;
    QLabel* destinationNote_ = nullptr;
    QComboBox* format_ = nullptr;
    QLineEdit* path_ = nullptr;
};
