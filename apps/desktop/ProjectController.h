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
#include <daw/session/BleedTools.h>
#include <daw/session/MasteringTools.h>
#include <daw/session/NoiseTools.h>
#include <daw/session/VoiceTools.h>
#include <daw/session/Destinations.h>
#include <daw/session/DrumMidi.h>
#include <daw/session/Renderer.h>
#include <daw/session/StemReport.h>
#include <daw/session/UndoStack.h>

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
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

    /// SoundFont for MIDI tracks. Changing it re-renders the MIDI media.
    void setSoundFont(const QString& path);
    QString soundFont() const;
    void analyzeAll();

    // Stem report. Runs automatically once imported or opened stems are
    // loaded; mixer changes mark it out of date until it is run again.
    void runStemReport();
    const daw::StemReport* stemReport() const noexcept { return stemReport_ ? &*stemReport_ : nullptr; }
    bool isStemReportStale() const noexcept { return stemReportStale_; }
    bool isStemReportRunning() const noexcept { return stemReportRunning_; }

    // Undo/redo of every project edit below (and of imports).
    bool canUndo() const noexcept { return undo_.canUndo(); }
    bool canRedo() const noexcept { return undo_.canRedo(); }
    QString undoText() const { return QString::fromStdString(undo_.undoLabel()); }
    QString redoText() const { return QString::fromStdString(undo_.redoLabel()); }
    void undo();
    void redo();

    // Tracks.
    void renameTrack(const daw::TrackId& track, const QString& name);
    void removeTrack(const daw::TrackId& track);

    // Mixer. These update the model and the engine immediately.
    void setTrackGain(const daw::TrackId& track, double gainDb);
    void setTrackPan(const daw::TrackId& track, double pan);
    void setTrackMuted(const daw::TrackId& track, bool muted);
    /// `exclusive` un-solos every other track (plain click); false adds to the solo set (Ctrl+click).
    void setTrackSoloed(const daw::TrackId& track, bool soloed, bool exclusive = true);
    void setMasterGain(double gainDb);
    // The master bus (limiter, loudness target).
    void setMasterSettings(daw::MasterSettings settings);
    /// Sets the master fader for `targetLufs` through the limiter (switched
    /// on), in the background; loudnessMatched or loudnessMatchFailed follows.
    void matchLoudness(double targetLufs);
    /// Tonal targets saved in the app's data folder (TonalTargets/*.json).
    std::vector<daw::TonalTargetSettings> tonalTargetLibrary() const;
    /// Builds a target from the audio files in `folder` (background), saves
    /// it to the library, makes it the project's target and compares.
    void buildTonalTarget(const QString& folder);
    /// Measures the mix against the project's target (background).
    void compareTonalBalance();

    // Clean-up chains (live, non-destructive).
    void setTrackCleanup(const daw::TrackId& track, daw::TrackCleanup cleanup);
    /// Applies stem report fixes (track indices refer to the current report).
    void applyCleanupFixes(const std::vector<daw::CleanupFix>& fixes);
    /// A/B listening: plays every track without its clean-up. Not saved; renders
    /// and the stem report always include the clean-up.
    void setCleanupBypassed(bool bypassed);
    bool isCleanupBypassed() const noexcept { return cleanupBypassed_; }
    /// Plays only what the clean-up chains remove (tracks without one are
    /// silent). Not saved; turns the bypass off, and the bypass turns it off.
    void setListenRemoved(bool on);
    bool isListeningRemoved() const noexcept { return listenRemoved_; }
    /// Learns the track's noise profile in the background, from its quietest
    /// moments or from all of `region`, and switches its noise reduction on
    /// (one undo step).
    void learnNoiseProfile(const daw::TrackId& track, std::optional<daw::FrameRange> region = std::nullopt);
    /// Looks for mains hum in the background (in `region` only, when given);
    /// when found, switches the track's hum removal on at that frequency.
    void detectHum(const daw::TrackId& track, std::optional<daw::FrameRange> region = std::nullopt);
    /// Renders the track's original recording through the voice model in the
    /// background and makes the track play the result (one undo step). The
    /// model must be installed (daw::isVoiceModelInstalled).
    void voiceDenoise(const daw::TrackId& track, double reductionDb);
    /// Renders bleed-suppressed copies of `tracks`, each judged against every
    /// track of recorded audio, in the background, and makes them play the
    /// copies (one undo step).
    void suppressBleed(const std::vector<daw::TrackId>& tracks, const daw::BleedStrength& strength);
    /// The track plays its original recording again, undoing voice denoise or
    /// bleed suppression (one undo step).
    void revertProcessing(const daw::TrackId& track);
    /// A voice denoise or bleed suppression is rendering.
    bool isRenderRunning() const noexcept { return voiceJobs_ + bleedJobs_ > 0; }

    // Export metadata.
    /// Profile assigned to new projects (does not touch the open project).
    void setDefaultProfileId(const std::string& id) { defaultProfileId_ = id; }
    void setProfileId(const std::string& id, bool markDirty = true);
    void setReleaseMetadata(const daw::MetadataFields& fields);

    /// Renders and writes the mix in the background; reports via exportFinished.
    void exportMix(daw::ExportRequest request);
    /// One file per destination, each mastered for it (the project is not
    /// changed), one after the other; reports via exportFinished.
    struct DestinationFile {
        const daw::Destination* destination = nullptr;
        std::filesystem::path path;
    };
    void exportForDestinations(daw::ExportRequest request, std::vector<DestinationFile> files);

    /// Transcribes a drum track to a General MIDI file in the background,
    /// saves it (the project's Exports folder; Documents/Stem DAWg/MIDI for
    /// an untitled project) and imports it as a MIDI track. `grid`: notes per
    /// whole note to quantise to (16, 32), 0 = keep the played timing.
    void drumsToMidi(const daw::TrackId& track, int grid);

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
    void drumsToMidiFinished(bool ok, const QString& message, const QString& path);
    void stemReportProgress(double fraction);
    /// A new report arrived, it went out of date, or it started/stopped running.
    void stemReportChanged();
    void stemReportFailed(const QString& message);
    /// The undo or redo history changed (enable and label the menu items).
    void undoChanged();
    /// Voice denoise finished on a track and took away this much of its loudest
    /// parts (dB, negative): the model may be treating the voice as noise.
    void voiceLossWarning(const QString& trackName, double voiceChangeDb);
    /// Bleed suppression took this much (dB, negative) of the louder parts of
    /// these tracks: they may mostly duplicate other stems.
    void bleedLossWarning(const QStringList& trackNames, double worstChangeDb);
    void loudnessMatched(const daw::LoudnessMatch& match, double targetLufs);
    void loudnessMatchFailed(const QString& message);
    void tonalLibraryChanged();
    void tonalBusy(bool running, const QString& message);
    void tonalCompared(const daw::TonalComparison& comparison, const daw::TonalTargetSettings& target);
    void tonalFailed(const QString& message);

private:
    void resetSession();
    void onEngineRateChanged(double rate);
    void loadAllMedia();
    void loadMediaItems(const std::vector<daw::MediaRef>& items);
    void retargetAllMedia();
    void rebuildSnapshot();
    void setDirty(bool dirty);
    void jobStarted();
    void jobFinished();
    void autosave();
    void handleAnalysis(const daw::JobOutcome& outcome);
    void reloadAnalysisMarkers();
    void markStemReportStale();
    void maybeRunStemReport();
    bool allClipMediaLoaded() const;
    /// Applies an undoable edit: `change` mutates the model and returns whether
    /// it changed anything. Updates the engine and marks the report out of date.
    template <typename Change>
    bool applyEdit(const QString& label, const daw::UndoMerge& merge, Change change);
    template <typename Mutate>
    void mutateTrack(const daw::TrackId& id, const QString& label, const daw::UndoMerge& merge, Mutate mutate);
    void adoptRestoredModel(daw::ProjectModel model);
    struct ImportResult;
    struct PendingImport {
        std::uint64_t ticket = 0;
        QString file;
        std::string trackName; // shortened for batches ("Song (Bass)" -> "Bass")
        std::uint64_t batch = 0; // files imported together undo as one step
        std::shared_ptr<ImportResult> result; // null until the background work finishes
    };
    void flushImports();
    void applyImport(const PendingImport& import);
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
    std::uint64_t nextImportBatch_ = 0;
    daw::UndoStack undo_;
    QElapsedTimer undoClock_; // times merges of continuous edits

    std::string defaultProfileId_;
    std::filesystem::path soundFont_;
    double engineRate_ = 0.0;
    std::int64_t pendingPlayhead_ = -1; // project frames, applied when the engine is ready
    std::uint64_t generation_ = 0;      // invalidates results from a previous project
    std::uint64_t snapshotVersion_ = 0;
    std::shared_ptr<daw::VoiceDenoiser> voiceModel_; // loaded on first use
    int voiceJobs_ = 0;
    int bleedJobs_ = 0;
    bool cleanupBypassed_ = false;
    bool listenRemoved_ = false;
    int pendingJobs_ = 0;
    std::shared_ptr<daw::CancellationSource> cancel_;

    std::optional<daw::StemReport> stemReport_;
    bool stemReportStale_ = false;
    bool stemReportRunning_ = false;
    bool stemReportWanted_ = false; // run once everything is loaded
    std::uint64_t stemReportRun_ = 0;
    std::shared_ptr<daw::CancellationSource> stemCancel_;

    QTimer autosaveTimer_;
    QThreadPool workers_; // declared last so it is joined before other members die
};
