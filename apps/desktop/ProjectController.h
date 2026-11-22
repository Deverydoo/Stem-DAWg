// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Owns the open project: model, project directory, decoded media, analysis
// results and the mapping to the engine. All public methods run on the Qt main
// thread; heavy work (import, decode, resample, peaks) runs on a private
// thread pool and results are marshalled back with queued invocations.

#include <daw/ai/AnalysisService.h>
#include <daw/core/CancellationToken.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>
#include <daw/session/Renderer.h>

#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <deque>
#include <filesystem>
#include <memory>
#include <vector>

class EngineBridge;

class ProjectController final : public QObject {
    Q_OBJECT

public:
    ProjectController(EngineBridge& engine, daw::AnalysisService& analysis, QObject* parent = nullptr);
    ~ProjectController() override;

    const daw::ProjectModel& model() const noexcept { return model_; }
    const std::filesystem::path& root() const noexcept { return root_; }
    const daw::MediaPool& media() const noexcept { return pool_; }
    bool isUntitled() const noexcept { return untitled_; }
    bool isDirty() const noexcept { return dirty_; }
    bool isBusy() const noexcept { return pendingJobs_ > 0; }
    QString displayName() const;
    const std::vector<std::int64_t>& markers() const noexcept { return markers_; }

    void newProject();
    static bool hasRecoverableAutosave(const QString& directory);
    bool openProject(const QString& directory, bool recoverAutosave, QString* error);
    bool save(QString* error);
    bool saveAs(const QString& directory, QString* error);
    /// Imports files as new tracks. A batch is ordered naturally by file name
    /// ("2 Drums" before "10 FX") and tracks are added in that order even
    /// though the files decode in parallel.
    void importAudio(const QStringList& files);
    void analyzeAll();

    // Mixer. These update the model and the engine immediately (no undo yet).
    void setTrackGain(const daw::TrackId& track, double gainDb);
    void setTrackPan(const daw::TrackId& track, double pan);
    void setTrackMuted(const daw::TrackId& track, bool muted);
    void setTrackSoloed(const daw::TrackId& track, bool soloed);
    void setMasterGain(double gainDb);

    // Export metadata.
    /// Profile assigned to new projects (does not touch the open project).
    void setDefaultProfileId(const std::string& id) { defaultProfileId_ = id; }
    void setProfileId(const std::string& id, bool markDirty = true);
    void setReleaseMetadata(const daw::MetadataFields& fields);

    /// Renders and writes the mix in the background; reports via exportFinished.
    void exportMix(daw::ExportRequest request);

    std::int64_t playheadProjectFrames() const;
    void seekProjectFrames(std::int64_t frame);

Q_SIGNALS:
    void projectChanged();
    void mediaChanged();
    void dirtyChanged(bool dirty);
    void busyChanged(bool busy);
    void statusMessage(const QString& text);
    void analysisText(const QString& text);
    void markersChanged();
    /// Fader, pan, mute or solo changed (no structural change to the tracks).
    void mixerChanged();
    void exportProgress(double fraction);
    /// `warning`: written, but clipped or without the requested artwork.
    void exportFinished(bool ok, bool warning, const QString& message, const QString& path);

private:
    void resetSession();
    void onEngineRateChanged(double rate);
    void loadAllMedia();
    void retargetAllMedia();
    void rebuildSnapshot();
    void setDirty(bool dirty);
    void jobStarted();
    void jobFinished();
    void autosave();
    void handleAnalysis(const daw::JobOutcome& outcome);
    void reloadAnalysisMarkers();
    template <typename Mutate>
    void mutateTrack(const daw::TrackId& id, Mutate mutate);
    struct ImportResult;
    struct PendingImport {
        std::uint64_t ticket = 0;
        QString file;
        std::shared_ptr<ImportResult> result; // null until the background work finishes
    };
    void flushImports();
    void applyImport(const QString& file, ImportResult& result);
    template <typename Work, typename Done>
    void runInBackground(Work work, Done done);

    EngineBridge& engine_;
    daw::AnalysisService& analysis_;

    daw::ProjectModel model_;
    std::filesystem::path root_;
    bool untitled_ = true;
    bool dirty_ = false;
    daw::MediaPool pool_;
    std::vector<std::int64_t> markers_;
    std::deque<PendingImport> imports_; // submission order
    std::uint64_t nextImportTicket_ = 0;

    std::string defaultProfileId_;
    double engineRate_ = 0.0;
    std::int64_t pendingPlayhead_ = -1; // project frames, applied when the engine is ready
    std::uint64_t generation_ = 0;      // invalidates results from a previous project
    std::uint64_t snapshotVersion_ = 0;
    int pendingJobs_ = 0;
    std::shared_ptr<daw::CancellationSource> cancel_;

    QTimer autosaveTimer_;
    QThreadPool workers_; // declared last so it is joined before other members die
};
