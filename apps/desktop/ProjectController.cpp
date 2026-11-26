// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "ProjectController.h"

#include "EngineBridge.h"

#include <daw/ai/Analyzers.h>
#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/Midi.h>
#include <daw/project/ProjectSerializer.h>
#include <daw/project/ProjectStore.h>
#include <daw/session/MediaLoader.h>
#include <daw/session/NoiseTools.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/SnapshotBuilder.h>
#include <daw/session/UndoStack.h>

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <utility>

namespace fs = std::filesystem;

namespace {

fs::path toPath(const QString& text)
{
    return daw::utf8ToPath(text.toStdString());
}

QString toQString(const fs::path& path)
{
    return QString::fromStdString(daw::pathToUtf8(path));
}

QString summarise(const daw::AnalysisResult& result, const QString& mediaName)
{
    const auto& d = result.data;
    QString text = QStringLiteral("[%1] %2 (v%3, %4 ms)\n")
                       .arg(mediaName, QString::fromStdString(result.analyzerId))
                       .arg(result.analyzerVersion)
                       .arg(result.elapsedMs, 0, 'f', 1);
    if (result.analyzerId == daw::kLevelAnalyzerId) {
        int c = 0;
        for (const auto& ch : d["channels"])
            text += QStringLiteral("  ch%1  peak %2 dBFS  rms %3 dBFS  crest %4 dB  clipped %5\n")
                        .arg(++c)
                        .arg(ch["peakDbfs"].get<double>(), 0, 'f', 2)
                        .arg(ch["rmsDbfs"].get<double>(), 0, 'f', 2)
                        .arg(ch["crestDb"].get<double>(), 0, 'f', 2)
                        .arg(ch["clippedSamples"].get<long long>());
        const auto& corr = d["overall"]["stereoCorrelation"];
        if (!corr.is_null())
            text += QStringLiteral("  stereo correlation %1\n").arg(corr.get<double>(), 0, 'f', 3);
    } else if (result.analyzerId == daw::kTransientAnalyzerId) {
        text += QStringLiteral("  %1 transient candidates (shown as yellow markers)\n").arg(d["count"].get<long long>());
    } else if (result.analyzerId == daw::kSpectralAnalyzerId) {
        text += QStringLiteral("  centroid %1 Hz  harshness (2-5 kHz) %2 %\n")
                    .arg(d["centroidHz"].get<double>(), 0, 'f', 0)
                    .arg(d["harshnessRatio"].get<double>() * 100.0, 0, 'f', 1);
        for (const auto& band : d["bands"])
            text += QStringLiteral("    %1 %2 %\n")
                        .arg(QString::fromStdString(band["name"].get<std::string>()), -11)
                        .arg(band["fraction"].get<double>() * 100.0, 5, 'f', 1);
    } else if (result.analyzerId == daw::kInferenceProbeAnalyzerId) {
        text += QStringLiteral("  %1 on %2: %3 samples in %4 ms, max error %5\n")
                    .arg(QString::fromStdString(d["backend"].get<std::string>()),
                         QString::fromStdString(d["provider"].get<std::string>()))
                    .arg(d["samples"].get<long long>())
                    .arg(d["latencyMs"].get<double>(), 0, 'f', 2)
                    .arg(d["maxAbsError"].get<double>());
    }
    return text;
}

} // namespace

struct ProjectController::ImportResult {
    daw::Result<daw::MediaRef> ref = daw::makeError(daw::ErrorCode::Unknown, "not run");
    daw::Result<daw::LoadedMedia> loaded = daw::makeError(daw::ErrorCode::Unknown, "not run");
    std::optional<daw::MidiFileInfo> midi; // set for MIDI files
};

ProjectController::ProjectController(EngineBridge& engine, daw::AnalysisService& analysis, QObject* parent)
    : QObject(parent), engine_(engine), analysis_(analysis), cancel_(std::make_shared<daw::CancellationSource>()),
      stemCancel_(std::make_shared<daw::CancellationSource>())
{
    workers_.setMaxThreadCount(3); // decoding and loading, plus the stem report
    undoClock_.start();
    connect(&engine_, &EngineBridge::sampleRateChanged, this, &ProjectController::onEngineRateChanged);
    engineRate_ = engine_.sampleRate();

    autosaveTimer_.setInterval(60 * 1000);
    connect(&autosaveTimer_, &QTimer::timeout, this, &ProjectController::autosave);
    autosaveTimer_.start();

    newProject();
}

ProjectController::~ProjectController()
{
    autosaveTimer_.stop();
    cancel_->cancel();
    stemCancel_->cancel();
    analysis_.cancelAll();
    analysis_.waitIdle(std::chrono::seconds(10)); // callbacks target this object
    workers_.clear();
    workers_.waitForDone();
    if (untitled_) {
        std::error_code ec;
        fs::remove_all(root_, ec);
    } else if (!root_.empty()) {
        (void)daw::ProjectStore::removeUnusedProcessedMedia(root_); // copies kept only for undo
    }
}

QString ProjectController::displayName() const
{
    return QString::fromStdString(model_.name);
}

template <typename Work, typename Done>
void ProjectController::runInBackground(Work work, Done done)
{
    const std::uint64_t generation = generation_;
    jobStarted();
    workers_.start([this, generation, work = std::move(work), done = std::move(done)]() mutable {
        auto result = std::make_shared<decltype(work())>(work());
        QMetaObject::invokeMethod(
            this,
            [this, generation, result, done = std::move(done)]() mutable {
                jobFinished();
                if (generation == generation_)
                    done(*result);
            },
            Qt::QueuedConnection);
    });
}

void ProjectController::jobStarted()
{
    if (pendingJobs_++ == 0)
        Q_EMIT busyChanged(true);
}

void ProjectController::jobFinished()
{
    if (--pendingJobs_ == 0)
        Q_EMIT busyChanged(false);
}

void ProjectController::resetSession()
{
    cancel_->cancel();
    cancel_ = std::make_shared<daw::CancellationSource>();
    stemCancel_->cancel();
    stemCancel_ = std::make_shared<daw::CancellationSource>();
    stemReport_.reset();
    stemReportStale_ = false;
    stemReportRunning_ = false;
    stemReportWanted_ = false;
    ++generation_;
    voiceJobs_ = 0; // their results are dropped with the old project
    bleedJobs_ = 0;
    analysis_.cancelAll();
    engine_.stop();
    pool_.clear();
    markers_.clear();
    imports_.clear();
    undo_.clear();
    Q_EMIT undoChanged();
    pendingPlayhead_ = -1;
    if (untitled_ && !root_.empty()) {
        std::error_code ec;
        fs::remove_all(root_, ec);
    } else if (!root_.empty()) {
        (void)daw::ProjectStore::removeUnusedProcessedMedia(root_); // copies kept only for undo
    }
}

void ProjectController::newProject()
{
    resetSession();
    model_ = daw::makeEmptyProject("Untitled");
    model_.appVersion = DAW_APP_VERSION;
    model_.profileId = defaultProfileId_;
    root_ = daw::appDataDirectory() / "Untitled" / daw::utf8ToPath(model_.id.str());
    (void)daw::ProjectStore::createLayout(root_);
    untitled_ = true;
    setDirty(false);
    pendingPlayhead_ = 0;
    rebuildSnapshot();
    Q_EMIT projectChanged();
    Q_EMIT markersChanged();
    Q_EMIT analysisText({});
    Q_EMIT stemReportChanged();
}

bool ProjectController::hasRecoverableAutosave(const QString& directory)
{
    return daw::ProjectStore::findRecoverableAutosave(toPath(directory)).has_value();
}

bool ProjectController::openProject(const QString& directory, bool recoverAutosave, QString* error)
{
    const fs::path dir = toPath(directory);
    auto loaded = recoverAutosave ? daw::ProjectStore::loadAutosave(dir) : daw::ProjectStore::load(dir);
    if (!loaded) {
        if (error)
            *error = QString::fromStdString(loaded.error().describe());
        return false;
    }

    resetSession();
    model_ = std::move(*loaded);
    root_ = dir;
    untitled_ = false;
    const std::size_t fallBacks = daw::fallBackFromMissingProcessedMedia(model_, root_);
    setDirty(recoverAutosave || fallBacks > 0);
    pendingPlayhead_ = model_.playhead;
    rebuildSnapshot();
    Q_EMIT projectChanged();
    Q_EMIT analysisText({});
    Q_EMIT stemReportChanged();
    reloadAnalysisMarkers();
    stemReportWanted_ = true;
    loadAllMedia();
    maybeRunStemReport(); // a project without media
    if (fallBacks == 0)
        Q_EMIT statusMessage(tr("Opened %1").arg(directory));
    else
        Q_EMIT statusMessage((fallBacks == 1 ? tr("Opened %1. A clip plays its original recording: its processed copy is gone "
                                                  "(apply the clean-up again).")
                                             : tr("Opened %1. %2 clips play their original recording: their processed copies "
                                                  "are gone (apply the clean-up again)."))
                                 .arg(directory)
                                 .arg(fallBacks));
    return true;
}

bool ProjectController::save(QString* error)
{
    if (untitled_) {
        if (error)
            *error = tr("Untitled project: choose a location with Save As.");
        return false;
    }
    model_.playhead = playheadProjectFrames();
    model_.modifiedUtc = daw::utcTimestampIso8601();
    model_.appVersion = DAW_APP_VERSION;
    if (auto status = daw::ProjectStore::save(root_, model_); !status) {
        if (error)
            *error = QString::fromStdString(status.error().describe());
        return false;
    }
    setDirty(false);
    Q_EMIT statusMessage(tr("Saved %1").arg(toQString(root_)));
    return true;
}

bool ProjectController::saveAs(const QString& directory, QString* error)
{
    const fs::path target = toPath(directory);
    std::error_code ec;
    if (fs::exists(target, ec) && !fs::is_empty(target, ec) && !daw::ProjectStore::isProjectDirectory(target)) {
        if (error)
            *error = tr("The folder is not empty and is not a Stem DAWg project.");
        return false;
    }
    if (fs::weakly_canonical(target, ec) != fs::weakly_canonical(root_, ec)) {
        fs::create_directories(target, ec);
        for (const auto& entry : fs::directory_iterator(root_, ec)) {
            const auto name = entry.path().filename();
            if (name == "Backups" || name == daw::ProjectStore::kManifestName)
                continue;
            fs::copy(entry.path(), target / name, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                if (error)
                    *error = tr("Copying project files failed: %1").arg(QString::fromStdString(ec.message()));
                return false;
            }
        }
        if (untitled_)
            fs::remove_all(root_, ec);
    }
    root_ = target;
    untitled_ = false;
    model_.name = daw::pathToUtf8(target.filename());
    Q_EMIT projectChanged();
    return save(error);
}

void ProjectController::importAudio(const QStringList& files)
{
    QStringList ordered = files;
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(ordered.begin(), ordered.end(), [&](const QString& a, const QString& b) {
        return collator.compare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
    });

    std::vector<std::string> stems;
    for (const QString& file : ordered)
        stems.push_back(QFileInfo(file).completeBaseName().toStdString());
    const std::vector<std::string> names = daw::shortTrackNames(stems);

    const fs::path root = root_;
    const double rate = engineRate_;
    const fs::path soundFont = soundFont_;
    // MIDI renders at the project rate (the device rate for a new project).
    const double midiRate = !model_.media.empty() ? model_.sampleRate : (engineRate_ > 0.0 ? engineRate_ : 48000.0);
    const auto token = cancel_->token();
    const std::uint64_t batch = nextImportBatch_++;
    for (qsizetype i = 0; i < ordered.size(); ++i) {
        const QString& file = ordered[i];
        const std::uint64_t ticket = nextImportTicket_++;
        std::string name = names[static_cast<std::size_t>(i)];
        if (daw::isMidiFile(toPath(file)))
            name += " (MIDI)";
        imports_.push_back(PendingImport{ticket, file, name, batch, nullptr});
        const fs::path source = toPath(file);
        runInBackground(
            [root, source, rate, midiRate, soundFont, token] {
                auto out = std::make_shared<ImportResult>();
                if (daw::isMidiFile(source)) {
                    if (auto info = daw::probeMidiFile(source))
                        out->midi = *info;
                }
                out->ref = daw::importAudioFile(root, source, daw::MediaStorage::Copied, midiRate);
                if (out->ref) {
                    daw::MediaLoadOptions options;
                    options.engineSampleRate = rate;
                    options.soundFont = soundFont;
                    out->loaded = daw::loadMedia(root, *out->ref, options, token);
                }
                return out;
            },
            [this, ticket](std::shared_ptr<ImportResult>& result) {
                const auto it = std::find_if(imports_.begin(), imports_.end(),
                                             [&](const PendingImport& p) { return p.ticket == ticket; });
                if (it == imports_.end())
                    return;
                it->result = std::move(result);
                flushImports();
            });
    }
}

void ProjectController::flushImports()
{
    // Apply finished imports strictly in submission order.
    while (!imports_.empty() && imports_.front().result) {
        PendingImport next = std::move(imports_.front());
        imports_.pop_front();
        applyImport(next);
    }
    maybeRunStemReport();
}

void ProjectController::applyImport(const PendingImport& import)
{
    const QString& file = import.file;
    ImportResult& result = *import.result;
    if (!result.ref || !result.loaded) {
        const auto& err = !result.ref ? result.ref.error() : result.loaded.error();
        Q_EMIT statusMessage(tr("Import failed for %1: %2").arg(file, QString::fromStdString(err.describe())));
        daw::logging::warn("project", "import failed", {{"file", file.toStdString()}, {"error", err.describe()}});
        return;
    }
    // The whole batch is one undo step, however long the files take to load.
    undo_.record(model_, tr("Import").toStdString(), {"import:" + std::to_string(import.batch), daw::UndoMerge::kForever},
                 undoClock_.elapsed() / 1000.0);
    Q_EMIT undoChanged();
    // The first MIDI file of a project sets its tempo (Suno's MIDI follows the
    // song's tempo), so the bar.beat ruler lines up with the music.
    const bool hadMidi = std::any_of(model_.media.begin(), model_.media.end(),
                                     [&](const daw::MediaRef& m) { return daw::isMidiFile(daw::utf8ToPath(m.path)); });
    if (result.midi && !hadMidi && model_.tempo.bpm == daw::makeEmptyProject("x").tempo.bpm && result.midi->averageBpm > 20.0)
        model_.tempo.bpm = std::round(result.midi->averageBpm * 100.0) / 100.0;
    const daw::TrackId trackId = daw::addMediaAsNewTrack(model_, *result.ref);
    if (!import.trackName.empty()) {
        daw::Track* track = model_.findTrack(trackId);
        track->name = import.trackName;
        for (daw::Clip& clip : track->clips)
            clip.name = import.trackName;
    }
    pool_.put(std::move(*result.loaded));
    setDirty(true);
    markStemReportStale();
    stemReportWanted_ = true;
    if (engineRate_ != 0.0 && pool_.get(result.ref->id)->renditionRate != engineRate_)
        retargetAllMedia(); // device changed while importing
    rebuildSnapshot();
    Q_EMIT projectChanged();
    Q_EMIT mediaChanged();
    Q_EMIT statusMessage(tr("Imported %1").arg(QFileInfo(file).fileName()));
}

template <typename Change>
bool ProjectController::applyEdit(const QString& label, const daw::UndoMerge& merge, Change change)
{
    daw::ProjectModel before = model_;
    if (!change(model_))
        return false;
    undo_.record(before, label.toStdString(), merge, undoClock_.elapsed() / 1000.0);
    setDirty(true);
    rebuildSnapshot();
    markStemReportStale();
    Q_EMIT undoChanged();
    return true;
}

template <typename Mutate>
void ProjectController::mutateTrack(const daw::TrackId& id, const QString& label, const daw::UndoMerge& merge, Mutate mutate)
{
    const bool changed = applyEdit(label, merge, [&](daw::ProjectModel& model) {
        daw::Track* track = model.findTrack(id);
        return track != nullptr && mutate(*track);
    });
    if (changed)
        Q_EMIT mixerChanged();
}

void ProjectController::undo()
{
    if (auto restored = undo_.undo(model_)) {
        const QString label = redoText();
        adoptRestoredModel(std::move(*restored));
        Q_EMIT statusMessage(tr("Undid %1").arg(label));
    }
}

void ProjectController::redo()
{
    if (auto restored = undo_.redo(model_)) {
        const QString label = undoText();
        adoptRestoredModel(std::move(*restored));
        Q_EMIT statusMessage(tr("Redid %1").arg(label));
    }
}

void ProjectController::adoptRestoredModel(daw::ProjectModel model)
{
    model_ = std::move(model);
    setDirty(true);
    rebuildSnapshot();
    markStemReportStale();
    reloadAnalysisMarkers();
    Q_EMIT undoChanged();
    Q_EMIT projectChanged();
    Q_EMIT mediaChanged();
    Q_EMIT mixerChanged();
}

void ProjectController::renameTrack(const daw::TrackId& id, const QString& name)
{
    if (applyEdit(tr("Rename Track"), {}, [&](daw::ProjectModel& model) { return daw::renameTrack(model, id, name.toStdString()); }))
        Q_EMIT projectChanged();
}

void ProjectController::removeTrack(const daw::TrackId& id)
{
    const daw::Track* track = model_.findTrack(id);
    if (track == nullptr)
        return;
    const QString name = QString::fromStdString(track->name);
    if (applyEdit(tr("Delete Track"), {}, [&](daw::ProjectModel& model) { return daw::removeTrack(model, id); })) {
        reloadAnalysisMarkers();
        Q_EMIT projectChanged();
        Q_EMIT mediaChanged();
        Q_EMIT statusMessage(tr("Deleted %1. Ctrl+Z brings it back.").arg(name));
    }
}

void ProjectController::setTrackGain(const daw::TrackId& id, double gainDb)
{
    gainDb = std::clamp(gainDb, daw::kSilenceDb, 12.0);
    mutateTrack(id, tr("Volume"), {"gain:" + id.str()},
                [gainDb](daw::Track& t) { return std::exchange(t.gainDb, gainDb) != gainDb; });
}

void ProjectController::setTrackPan(const daw::TrackId& id, double pan)
{
    pan = std::clamp(pan, -1.0, 1.0);
    mutateTrack(id, tr("Pan"), {"pan:" + id.str()}, [pan](daw::Track& t) { return std::exchange(t.pan, pan) != pan; });
}

void ProjectController::setTrackMuted(const daw::TrackId& id, bool muted)
{
    mutateTrack(id, muted ? tr("Mute") : tr("Unmute"), {},
                [muted](daw::Track& t) { return std::exchange(t.muted, muted) != muted; });
}

void ProjectController::setTrackSoloed(const daw::TrackId& id, bool soloed, bool exclusive)
{
    if (applyEdit(soloed ? tr("Solo") : tr("Unsolo"), {},
                  [&](daw::ProjectModel& model) { return daw::setTrackSolo(model, id, soloed, exclusive); }))
        Q_EMIT mixerChanged(); // also refreshes the other headers' solo buttons
}

void ProjectController::setMasterGain(double gainDb)
{
    gainDb = std::clamp(gainDb, daw::kSilenceDb, 12.0);
    if (applyEdit(tr("Master Volume"), {"master"},
                  [gainDb](daw::ProjectModel& model) { return std::exchange(model.masterGainDb, gainDb) != gainDb; }))
        Q_EMIT mixerChanged();
}

void ProjectController::setMasterSettings(daw::MasterSettings settings)
{
    settings.clamp();
    if (applyEdit(tr("Master Bus"), {"master-bus"},
                  [&settings](daw::ProjectModel& model) { return std::exchange(model.master, settings) != settings; }))
        Q_EMIT mixerChanged();
}

void ProjectController::matchLoudness(double targetLufs)
{
    const daw::ProjectModel model = model_;
    daw::MasterSettings master = model_.master;
    master.limiter = true;
    const auto token = cancel_->token();
    Q_EMIT statusMessage(tr("Matching %1 LUFS...").arg(targetLufs, 0, 'f', 1));
    runInBackground([this, model, targetLufs, master, token] { return daw::matchLoudness(model, pool_, targetLufs, master, {}, token); },
                    [this, targetLufs, master](daw::Result<daw::LoudnessMatch>& match) {
                        if (!match) {
                            if (match.error().code != daw::ErrorCode::Cancelled) {
                                const QString message = tr("Could not match the loudness: %1").arg(QString::fromStdString(match.error().message));
                                Q_EMIT statusMessage(message);
                                Q_EMIT loudnessMatchFailed(message);
                            }
                            return;
                        }
                        const double gain = std::clamp(match->masterGainDb, daw::kSilenceDb, 12.0);
                        applyEdit(tr("Match Loudness"), {}, [&](daw::ProjectModel& m) {
                            m.masterGainDb = gain;
                            m.master = daw::applyLoudnessMatch(master, *match, targetLufs);
                            return true;
                        });
                        Q_EMIT mixerChanged();
                        Q_EMIT loudnessMatched(*match, targetLufs);
                        Q_EMIT statusMessage(match->reached
                                                 ? tr("Master fader set to %1 dB: the master measures %2 LUFS.").arg(gain, 0, 'f', 1).arg(match->resultLufs, 0, 'f', 1)
                                                 : tr("Master fader set to %1 dB: %2 LUFS, short of the %3 LUFS target.")
                                                       .arg(gain, 0, 'f', 1)
                                                       .arg(match->resultLufs, 0, 'f', 1)
                                                       .arg(targetLufs, 0, 'f', 1));
                    });
}

namespace {

std::filesystem::path tonalLibraryDirectory()
{
    return daw::appDataDirectory() / "TonalTargets";
}

} // namespace

std::vector<daw::TonalTargetSettings> ProjectController::tonalTargetLibrary() const
{
    std::vector<daw::TonalTargetSettings> targets;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(tonalLibraryDirectory(), ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const auto doc = nlohmann::json::parse(in, nullptr, false);
        daw::TonalTargetSettings t = daw::tonalTargetFromJson(doc);
        if (!t.empty())
            targets.push_back(std::move(t));
    }
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return targets;
}

void ProjectController::buildTonalTarget(const QString& folder)
{
    const std::filesystem::path dir = daw::utf8ToPath(folder.toStdString());
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const auto extensions = daw::supportedAudioExtensions();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = daw::pathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end())
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        Q_EMIT tonalFailed(tr("No audio files in %1").arg(folder));
        return;
    }
    const std::string name = daw::pathToUtf8(dir.filename());
    Q_EMIT tonalBusy(true, tr("Measuring %n reference track(s)...", nullptr, static_cast<int>(files.size())));
    const auto token = cancel_->token();
    runInBackground(
        [name, files, token] {
            std::vector<std::string> skipped;
            auto target = daw::tonalTargetFromFiles(name, files, &skipped, {}, token);
            return std::make_pair(std::move(target), std::move(skipped));
        },
        [this, name](std::pair<daw::Result<daw::TonalTarget>, std::vector<std::string>>& outcome) {
            Q_EMIT tonalBusy(false, {});
            auto& target = outcome.first;
            if (!target) {
                if (target.error().code != daw::ErrorCode::Cancelled)
                    Q_EMIT tonalFailed(tr("Could not build the target: %1").arg(QString::fromStdString(target.error().message)));
                return;
            }
            const daw::TonalTargetSettings settings = daw::toSettings(*target);
            std::error_code ec;
            std::filesystem::create_directories(tonalLibraryDirectory(), ec);
            std::string file = name;
            for (char& c : file)
                if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
                    c = '_';
            if (!daw::writeFileAtomically(tonalLibraryDirectory() / daw::utf8ToPath(file + ".json"), daw::tonalTargetToJson(settings).dump(2)))
                Q_EMIT statusMessage(tr("Could not save the target to the library; it stays in this project."));
            Q_EMIT tonalLibraryChanged();
            applyEdit(tr("Tonal Target"), {}, [&](daw::ProjectModel& m) {
                return std::exchange(m.master.tonalTarget, settings) != settings;
            });
            Q_EMIT mixerChanged();
            if (!outcome.second.empty())
                Q_EMIT statusMessage(tr("Skipped %n file(s) that could not be read.", nullptr, static_cast<int>(outcome.second.size())));
            compareTonalBalance();
        });
}

void ProjectController::compareTonalBalance()
{
    const daw::TonalTargetSettings target = model_.master.tonalTarget;
    if (target.empty()) {
        Q_EMIT tonalFailed(tr("Choose a tonal target first."));
        return;
    }
    std::array<float, daw::kMasterEqBandCount> eq{};
    for (std::size_t b = 0; b < eq.size(); ++b)
        eq[b] = model_.master.eq ? static_cast<float>(model_.master.eqGainDb[b]) : 0.0f;
    const daw::ProjectModel model = model_;
    const auto token = cancel_->token();
    Q_EMIT tonalBusy(true, tr("Measuring the mix..."));
    runInBackground([this, model, token] { return daw::tonalProfileOfMix(model, pool_, {}, token); },
                    [this, target, eq](daw::Result<daw::TonalProfile>& profile) {
                        Q_EMIT tonalBusy(false, {});
                        if (!profile) {
                            if (profile.error().code != daw::ErrorCode::Cancelled)
                                Q_EMIT tonalFailed(tr("Could not measure the mix: %1").arg(QString::fromStdString(profile.error().message)));
                            return;
                        }
                        Q_EMIT tonalCompared(daw::compareTonal(*profile, daw::fromSettings(target), eq), target);
                    });
}

void ProjectController::setTrackCleanup(const daw::TrackId& id, daw::TrackCleanup cleanup)
{
    cleanup.clamp();
    mutateTrack(id, tr("Clean-up"), {"cleanup:" + id.str()},
                [&cleanup](daw::Track& t) { return std::exchange(t.cleanup, cleanup) != cleanup; });
}

void ProjectController::applyCleanupFixes(const std::vector<daw::CleanupFix>& fixes)
{
    if (!stemReport_ || fixes.empty())
        return;
    std::size_t changed = 0;
    applyEdit(tr("Apply Clean-up Fixes"), {}, [&](daw::ProjectModel& model) {
        changed = daw::applyCleanupFixes(model, *stemReport_, fixes);
        return changed > 0;
    });
    if (changed == 0) {
        Q_EMIT statusMessage(tr("Those clean-up settings are already in place"));
        return;
    }
    Q_EMIT mixerChanged();
    Q_EMIT statusMessage(tr("Clean-up set on %n track(s). Analyze again to measure the result.", nullptr, static_cast<int>(changed)));
}

void ProjectController::setCleanupBypassed(bool bypassed)
{
    if (cleanupBypassed_ == bypassed)
        return;
    cleanupBypassed_ = bypassed;
    if (bypassed)
        listenRemoved_ = false;
    rebuildSnapshot();
    Q_EMIT mixerChanged();
}

void ProjectController::setListenRemoved(bool on)
{
    if (listenRemoved_ == on)
        return;
    listenRemoved_ = on;
    if (on)
        cleanupBypassed_ = false;
    rebuildSnapshot();
    Q_EMIT mixerChanged();
}

void ProjectController::learnNoiseProfile(const daw::TrackId& id, std::optional<daw::FrameRange> region)
{
    const daw::Track* track = model_.findTrack(id);
    if (track == nullptr)
        return;
    const QString name = QString::fromStdString(track->name);
    Q_EMIT statusMessage(region ? tr("Learning the noise of %1 from the selection...").arg(name)
                                : tr("Learning the noise of %1...").arg(name));
    const daw::ProjectModel model = model_;
    const auto token = cancel_->token();
    daw::NoiseLearnOptions options;
    options.region = region;
    runInBackground([this, model, id, options, token] { return daw::learnNoiseProfile(model, pool_, id, options, token); },
                    [this, id, name, region](daw::Result<daw::NoiseProfile>& profile) {
                        if (!profile) {
                            if (profile.error().code != daw::ErrorCode::Cancelled)
                                Q_EMIT statusMessage(tr("Could not learn the noise of %1: %2")
                                                         .arg(name, QString::fromStdString(profile.error().message)));
                            return;
                        }
                        const double seconds = profile->seconds;
                        const bool changed = applyEdit(tr("Learn Noise Profile"), {}, [&](daw::ProjectModel& m) {
                            daw::Track* t = m.findTrack(id);
                            if (t == nullptr)
                                return false;
                            t->cleanup.bypassed = false;
                            t->cleanup.denoise.enabled = true;
                            t->cleanup.denoise.profile = std::move(*profile);
                            return true;
                        });
                        if (!changed)
                            return;
                        Q_EMIT mixerChanged();
                        Q_EMIT statusMessage((region ? tr("Learned the noise of %1 from %2 s of the selection. Noise reduction is on.")
                                                     : tr("Learned the noise of %1 from %2 s of its quietest audio. Noise reduction is on."))
                                                 .arg(name)
                                                 .arg(seconds, 0, 'f', 1));
                    });
}

void ProjectController::detectHum(const daw::TrackId& id, std::optional<daw::FrameRange> region)
{
    const daw::Track* track = model_.findTrack(id);
    if (track == nullptr)
        return;
    const QString name = QString::fromStdString(track->name);
    Q_EMIT statusMessage(tr("Looking for hum in %1...").arg(name));
    const daw::ProjectModel model = model_;
    const auto token = cancel_->token();
    runInBackground([this, model, id, region, token] { return daw::detectHum(model, pool_, id, token, region); },
                    [this, id, name, region](daw::Result<std::optional<daw::HumDetection>>& found) {
                        if (!found) {
                            if (found.error().code != daw::ErrorCode::Cancelled)
                                Q_EMIT statusMessage(tr("Could not check %1 for hum: %2")
                                                         .arg(name, QString::fromStdString(found.error().message)));
                            return;
                        }
                        if (!found->has_value()) {
                            Q_EMIT statusMessage(region ? tr("No mains hum found in the selection of %1.").arg(name)
                                                        : tr("No mains hum found in %1.").arg(name));
                            return;
                        }
                        const daw::HumDetection hum = **found;
                        applyEdit(tr("Detect Hum"), {}, [&](daw::ProjectModel& m) {
                            daw::Track* t = m.findTrack(id);
                            if (t == nullptr)
                                return false;
                            t->cleanup.bypassed = false;
                            t->cleanup.hum = {true, hum.frequencyHz, hum.harmonics};
                            return true;
                        });
                        Q_EMIT mixerChanged();
                        Q_EMIT statusMessage(tr("Found %1 Hz hum in %2 (%3 harmonics, %4 dB above the noise). Hum removal is on.")
                                                 .arg(hum.frequencyHz, 0, 'f', 0)
                                                 .arg(name)
                                                 .arg(hum.harmonics)
                                                 .arg(hum.prominenceDb, 0, 'f', 0));
                    });
}

void ProjectController::voiceDenoise(const daw::TrackId& id, double reductionDb)
{
    const daw::Track* track = model_.findTrack(id);
    if (track == nullptr)
        return;
    const QString name = QString::fromStdString(track->name);
    struct Input {
        daw::MediaRef media;
        daw::AudioAssetPtr audio;
    };
    std::vector<Input> inputs;
    for (const daw::MediaRef& media : daw::trackOriginalMedia(model_, id)) {
        const auto loaded = pool_.get(media.id);
        if (!loaded || !loaded->original) {
            Q_EMIT statusMessage(tr("%1 is still loading; try again in a moment.").arg(name));
            return;
        }
        inputs.push_back({media, loaded->original});
    }
    if (inputs.empty())
        return;

    struct Output {
        std::shared_ptr<daw::VoiceDenoiser> model;
        std::vector<daw::VoiceDenoiseOutcome> outcomes;
        std::optional<daw::Error> error;
    };
    Q_EMIT statusMessage(tr("Voice denoise: %1...").arg(name));
    ++voiceJobs_;
    const std::filesystem::path root = root_;
    const double rate = engineRate_;
    const auto token = cancel_->token();
    QPointer<ProjectController> self(this);
    runInBackground(
        [self, model = voiceModel_, inputs, root, rate, reductionDb, token, name]() mutable {
            Output out;
            if (!model) {
                auto backend = daw::createOnnxBackend({.intraOpThreads = 4});
                auto loaded = backend ? daw::VoiceDenoiser::load(*backend, daw::voiceModelDirectory())
                                      : daw::Result<std::shared_ptr<daw::VoiceDenoiser>>(backend.error());
                if (!loaded) {
                    out.error = loaded.error();
                    return out;
                }
                model = *loaded;
            }
            out.model = model;
            int lastPercent = -1;
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                auto outcome = daw::denoiseVoiceMedia(root, inputs[i].media, *inputs[i].audio, *model, reductionDb, rate, token,
                                                      [&](double p) {
                                                          const int percent = static_cast<int>(100.0 * (i + p) / inputs.size());
                                                          if (percent / 10 == lastPercent / 10)
                                                              return;
                                                          lastPercent = percent;
                                                          QMetaObject::invokeMethod(qApp, [self, name, percent] {
                                                              if (self)
                                                                  Q_EMIT self->statusMessage(tr("Voice denoise: %1 %2%").arg(name).arg(percent));
                                                          });
                                                      });
                if (!outcome) {
                    out.error = outcome.error();
                    return out;
                }
                out.outcomes.push_back(std::move(*outcome));
            }
            return out;
        },
        [this, id, name, reductionDb](Output& out) {
            --voiceJobs_;
            if (out.model)
                voiceModel_ = out.model;
            if (out.error) {
                if (out.error->code != daw::ErrorCode::Cancelled)
                    Q_EMIT statusMessage(tr("Voice denoise failed for %1: %2").arg(name, QString::fromStdString(out.error->message)));
                Q_EMIT mixerChanged();
                return;
            }
            std::vector<daw::MediaRef> copies;
            double voiceChange = 0.0, noiseChange = 0.0;
            for (daw::VoiceDenoiseOutcome& outcome : out.outcomes) {
                copies.push_back(outcome.media);
                voiceChange = std::min(voiceChange, outcome.voiceChangeDb);
                noiseChange = std::min(noiseChange, outcome.noiseChangeDb);
                pool_.put(std::move(outcome.loaded));
            }
            const bool changed = applyEdit(tr("Voice Denoise"), {}, [&](daw::ProjectModel& m) { return daw::applyProcessedMedia(m, id, copies); });
            if (engineRate_ != 0.0)
                retargetAllMedia(); // the device may have changed while it ran
            Q_EMIT mixerChanged();
            if (!changed)
                return;
            Q_EMIT projectChanged();
            Q_EMIT mediaChanged();
            Q_EMIT statusMessage(tr("Voice denoise on %1: the noise dropped by %2 dB, the voice changed by %3 dB. Ctrl+Z undoes it.")
                                     .arg(name)
                                     .arg(-noiseChange, 0, 'f', 0)
                                     .arg(voiceChange, 0, 'f', 1));
            if (voiceChange < daw::kVoiceLossWarningDb)
                Q_EMIT voiceLossWarning(name, voiceChange);
        });
    Q_EMIT mixerChanged();
}

void ProjectController::suppressBleed(const std::vector<daw::TrackId>& tracks, const daw::BleedStrength& strength)
{
    if (tracks.empty() || isRenderRunning())
        return;
    // Every original taking part must be decoded; the job holds them.
    std::map<daw::MediaId, daw::AudioAssetPtr> originals;
    for (const daw::TrackId& id : daw::bleedTracks(model_))
        for (const daw::MediaRef& media : daw::trackOriginalMedia(model_, id)) {
            const auto loaded = pool_.get(media.id);
            if (!loaded || !loaded->original) {
                Q_EMIT statusMessage(tr("The stems are still loading; try again in a moment."));
                return;
            }
            originals[media.id] = loaded->original;
        }
    struct Output {
        std::vector<daw::BleedTrackOutcome> outcomes;
        std::optional<daw::Error> error;
    };
    const QString label = tracks.size() == 1 && model_.findTrack(tracks[0]) != nullptr
                              ? QString::fromStdString(model_.findTrack(tracks[0])->name)
                              : tr("%1 stems").arg(tracks.size());
    Q_EMIT statusMessage(tr("Bleed suppression: %1...").arg(label));
    ++bleedJobs_;
    const std::filesystem::path root = root_;
    const double rate = engineRate_;
    const auto token = cancel_->token();
    const daw::ProjectModel snapshot = model_;
    const daw::BleedStrength* chosen = &strength;
    QPointer<ProjectController> self(this);
    runInBackground(
        [self, snapshot, tracks, root, rate, chosen, token, label, originals = std::move(originals)]() {
            Output out;
            int lastPercent = -1;
            auto result = daw::suppressBleed(
                root, snapshot,
                [&originals](const daw::MediaId& id) {
                    const auto it = originals.find(id);
                    return it == originals.end() ? daw::AudioAssetPtr{} : it->second;
                },
                tracks, *chosen, rate, token, [&](double p) {
                    const int percent = static_cast<int>(100.0 * p);
                    if (percent / 10 == lastPercent / 10)
                        return;
                    lastPercent = percent;
                    QMetaObject::invokeMethod(qApp, [self, label, percent] {
                        if (self)
                            Q_EMIT self->statusMessage(tr("Bleed suppression: %1 %2%").arg(label).arg(percent));
                    });
                });
            if (result)
                out.outcomes = std::move(*result);
            else
                out.error = result.error();
            return out;
        },
        [this, label, chosen](Output& out) {
            --bleedJobs_;
            if (out.error) {
                if (out.error->code != daw::ErrorCode::Cancelled)
                    Q_EMIT statusMessage(tr("Bleed suppression failed: %1").arg(QString::fromStdString(out.error->message)));
                Q_EMIT mixerChanged();
                return;
            }
            double worstMusic = 0.0, quietest = 0.0;
            QStringList losing;
            for (daw::BleedTrackOutcome& outcome : out.outcomes) {
                worstMusic = std::min(worstMusic, outcome.musicChangeDb);
                quietest = std::min(quietest, outcome.bleedChangeDb);
                if (outcome.musicChangeDb < daw::kBleedLossWarningDb)
                    if (const daw::Track* track = model_.findTrack(outcome.track))
                        losing << QString::fromStdString(track->name);
                for (daw::LoadedMedia& loaded : outcome.loaded)
                    pool_.put(std::move(loaded));
            }
            const bool changed = applyEdit(tr("Bleed Suppression"), {}, [&](daw::ProjectModel& m) {
                bool any = false;
                for (const daw::BleedTrackOutcome& outcome : out.outcomes)
                    any = daw::applyProcessedMedia(m, outcome.track, outcome.media) || any;
                return any;
            });
            if (engineRate_ != 0.0)
                retargetAllMedia(); // the device may have changed while it ran
            Q_EMIT mixerChanged();
            if (!changed)
                return;
            Q_EMIT projectChanged();
            Q_EMIT mediaChanged();
            Q_EMIT statusMessage(tr("Bleed suppression (%1) on %2: the quiet parts dropped by up to %3 dB, the music changed by "
                                    "at most %4 dB. Ctrl+Z undoes it.")
                                     .arg(tr(chosen->name), label)
                                     .arg(-quietest, 0, 'f', 0)
                                     .arg(worstMusic, 0, 'f', 1));
            if (!losing.isEmpty())
                Q_EMIT bleedLossWarning(losing, worstMusic);
        });
    Q_EMIT mixerChanged();
}

void ProjectController::revertProcessing(const daw::TrackId& id)
{
    const daw::Track* track = model_.findTrack(id);
    if (track == nullptr)
        return;
    const QString name = QString::fromStdString(track->name);
    if (applyEdit(tr("Original Recording"), {}, [&](daw::ProjectModel& m) { return daw::revertTrackProcessing(m, id); })) {
        Q_EMIT projectChanged();
        Q_EMIT mediaChanged();
        Q_EMIT mixerChanged();
        Q_EMIT statusMessage(tr("%1 plays its original recording again.").arg(name));
    }
}

void ProjectController::setProfileId(const std::string& id, bool markDirty)
{
    if (model_.profileId == id)
        return;
    if (!markDirty) { // assigning the default profile to a new project is not an edit
        model_.profileId = id;
        return;
    }
    const daw::ProjectModel before = model_;
    model_.profileId = id;
    undo_.record(before, tr("Artist Profile").toStdString(), {}, undoClock_.elapsed() / 1000.0);
    setDirty(true);
    Q_EMIT undoChanged();
}

void ProjectController::setReleaseMetadata(const daw::MetadataFields& fields)
{
    if (model_.release == fields)
        return;
    const daw::ProjectModel before = model_;
    model_.release = fields;
    undo_.record(before, tr("Song Details").toStdString(), {}, undoClock_.elapsed() / 1000.0);
    setDirty(true);
    Q_EMIT undoChanged();
}

void ProjectController::exportMix(daw::ExportRequest request)
{
    const daw::ProjectModel model = model_;
    const auto token = cancel_->token();
    QPointer<ProjectController> self(this);
    runInBackground(
        [this, self, model, request, token]() {
            daw::RenderOptions options;
            options.progress = [this, self](double p) {
                QMetaObject::invokeMethod(this, [self, p] { if (self) Q_EMIT self->exportProgress(p); }, Qt::QueuedConnection);
            };
            return daw::exportMix(model, pool_, request, options, token);
        },
        [this, path = request.path](daw::Result<daw::ExportReport>& result) {
            const QString file = toQString(path);
            if (!result) {
                daw::logging::warn("export", "export failed", {{"file", file.toStdString()}, {"error", result.error().describe()}});
                Q_EMIT exportFinished(false, true, QString::fromStdString(result.error().describe()), file);
                return;
            }
            const auto& r = *result;
            QString message = tr("Exported %1 (%2 MB, %3 s of audio, peak %4 dBFS).")
                                  .arg(QFileInfo(file).fileName())
                                  .arg(static_cast<double>(r.bytes) / (1024.0 * 1024.0), 0, 'f', 1)
                                  .arg(static_cast<double>(r.render.frames) / r.render.sampleRate, 0, 'f', 1)
                                  .arg(r.render.peakDbfs, 0, 'f', 1);
            const auto& loudness = r.render.loudness;
            if (loudness.integratedLufs > -70.0)
                message += QStringLiteral("\n") + tr("Loudness %1 LUFS integrated, range %2 LU, true peak %3 dBTP.")
                                                   .arg(loudness.integratedLufs, 0, 'f', 1)
                                                   .arg(loudness.loudnessRangeLu, 0, 'f', 1)
                                                   .arg(loudness.truePeakDbtp, 0, 'f', 1);
            if (r.render.clippedSamples > 0)
                message += QStringLiteral("\n\n")
                           + tr("Warning: %1 samples went over 0 dBFS and clipped. Lower the Master fader by at least %2 dB "
                                "and export again (or export 32-bit float to keep the overs).")
                                 .arg(r.render.clippedSamples)
                                 .arg(std::ceil(r.render.peakDbfs * 10.0) / 10.0, 0, 'f', 1);
            if (!r.artworkWarning.empty())
                message += QStringLiteral("\n\n") + tr("Artwork was not embedded: %1").arg(QString::fromStdString(r.artworkWarning));
            daw::logging::info("export", "exported",
                               {{"file", file.toStdString()},
                                {"bytes", r.bytes},
                                {"peakDbfs", r.render.peakDbfs},
                                {"lufs", r.render.loudness.integratedLufs},
                                {"truePeakDbtp", r.render.loudness.truePeakDbtp},
                                {"clipped", r.render.clippedSamples},
                                {"renderSeconds", r.render.renderSeconds}});
            Q_EMIT exportFinished(true, r.render.clippedSamples > 0 || !r.artworkWarning.empty(), message, file);
        });
}

void ProjectController::exportForDestinations(daw::ExportRequest request, std::vector<DestinationFile> files)
{
    struct Outcome {
        DestinationFile file;
        std::optional<daw::DestinationExportReport> report;
        std::string error;
    };
    const daw::ProjectModel model = model_;
    const auto token = cancel_->token();
    QPointer<ProjectController> self(this);
    runInBackground(
        [this, self, model, request, files, token]() -> daw::Result<std::vector<Outcome>> {
            std::vector<Outcome> outcomes;
            const auto count = static_cast<double>(files.size());
            for (std::size_t i = 0; i < files.size(); ++i) {
                daw::RenderOptions options;
                options.progress = [this, self, i, count](double p) {
                    const double overall = (static_cast<double>(i) + p) / count;
                    QMetaObject::invokeMethod(this, [self, overall] { if (self) Q_EMIT self->exportProgress(overall); }, Qt::QueuedConnection);
                };
                daw::ExportRequest fileRequest = request;
                fileRequest.path = files[i].path;
                auto report = daw::exportForDestination(model, pool_, *files[i].destination, fileRequest, options, token);
                if (!report && report.error().code == daw::ErrorCode::Cancelled)
                    return report.error();
                Outcome outcome{files[i], {}, {}};
                if (report)
                    outcome.report = std::move(*report);
                else
                    outcome.error = report.error().describe();
                outcomes.push_back(std::move(outcome));
            }
            return outcomes;
        },
        [this](daw::Result<std::vector<Outcome>>& result) {
            if (!result) {
                Q_EMIT exportFinished(false, true, QString::fromStdString(result.error().describe()), {});
                return;
            }
            QStringList lines;
            QString firstFile;
            bool anyWritten = false, warning = false;
            QString artworkWarning;
            for (const Outcome& o : *result) {
                const QString file = toQString(o.file.path);
                const QString name = QFileInfo(file).fileName();
                const QString key = QString::fromUtf8(o.file.destination->key);
                if (!o.report) {
                    warning = true;
                    lines << tr("%1: failed: %2").arg(key, QString::fromStdString(o.error));
                    daw::logging::warn("export", "destination export failed", {{"destination", o.file.destination->key}, {"error", o.error}});
                    continue;
                }
                if (firstFile.isEmpty())
                    firstFile = file;
                anyWritten = true;
                const daw::LoudnessMatch& m = o.report->match;
                const daw::RenderReport& r = o.report->file.render;
                const int bits = o.file.destination->format == daw::WavSampleFormat::Pcm16 ? 16 : 24;
                QString line = tr("%1: %2\n    %3 LUFS, true peak %4 dBTP, %5-bit %6 kHz, master fader %7 dB.")
                                   .arg(key, name)
                                   .arg(r.loudness.integratedLufs, 0, 'f', 1)
                                   .arg(r.loudness.truePeakDbtp, 0, 'f', 1)
                                   .arg(bits)
                                   .arg(r.sampleRate / 1000.0, 0, 'g', 3)
                                   .arg(std::clamp(m.masterGainDb, daw::kSilenceDb, 12.0), 0, 'f', 1);
                if (m.clipper)
                    line += QStringLiteral(" ") + tr("Soft clipper on (rounds off %1 % of the samples).").arg(m.clippedShare * 100.0, 0, 'f', 1);
                if (!m.reached) {
                    warning = true;
                    line += QStringLiteral("\n    ")
                            + tr("Short of the %1 LUFS target: louder would take more limiting or clipping than the mix can stand.")
                                  .arg(o.file.destination->lufs, 0, 'f', 0);
                }
                lines << line;
                if (!o.report->file.artworkWarning.empty())
                    artworkWarning = QString::fromStdString(o.report->file.artworkWarning);
                daw::logging::info("export", "exported for destination",
                                   {{"destination", o.file.destination->key},
                                    {"file", file.toStdString()},
                                    {"lufs", r.loudness.integratedLufs},
                                    {"truePeakDbtp", r.loudness.truePeakDbtp},
                                    {"masterGainDb", m.masterGainDb},
                                    {"clipper", m.clipper},
                                    {"reached", m.reached},
                                    {"renderSeconds", r.renderSeconds}});
            }
            QString message = result->size() == 1 ? tr("Exported, mastered for the destination:") : tr("Exported %n file(s), each mastered for its destination:", nullptr, static_cast<int>(result->size()));
            message += QStringLiteral("\n\n") + lines.join(QStringLiteral("\n\n"));
            message += QStringLiteral("\n\n") + tr("Your project's master settings are unchanged.");
            if (!artworkWarning.isEmpty()) {
                warning = true;
                message += QStringLiteral("\n\n") + tr("Artwork was not embedded: %1").arg(artworkWarning);
            }
            Q_EMIT exportFinished(anyWritten, warning, message, firstFile);
        });
}

void ProjectController::drumsToMidi(const daw::TrackId& trackId, int grid)
{
    const daw::Track* track = model_.findTrack(trackId);
    if (track == nullptr)
        return;
    const daw::ProjectModel model = model_;
    const auto token = cancel_->token();
    const QString trackName = QString::fromStdString(track->name);
    // Where the file goes: the project's Exports folder, or for an untitled
    // project (which lives in the app's data folder) Documents/Stem DAWg/MIDI.
    QString folder = untitled_ ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Stem DAWg/MIDI")
                               : toQString(root_ / "Exports");
    QDir().mkpath(folder);
    QString base = trackName;
    for (QChar& c : base)
        if (QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    const QString path = QDir(folder).filePath(base.trimmed() + QStringLiteral(" (drums).mid"));
    Q_EMIT statusMessage(tr("Transcribing %1 to MIDI...").arg(trackName));
    runInBackground(
        [this, model, trackId, grid, token, path]() -> daw::Result<daw::DrumMidiResult> {
            daw::DrumMidiOptions options;
            options.grid = grid;
            auto result = daw::trackDrumsToMidi(model, pool_, trackId, options, token);
            if (!result)
                return result;
            if (auto status = daw::writeMidiFile(toPath(path), result->song); !status)
                return status.error();
            return result;
        },
        [this, path, trackName, grid](daw::Result<daw::DrumMidiResult>& result) {
            if (!result) {
                if (result.error().code != daw::ErrorCode::Cancelled)
                    Q_EMIT drumsToMidiFinished(false, tr("Could not transcribe %1: %2").arg(trackName, QString::fromStdString(result.error().message)), {});
                return;
            }
            int counts[daw::kDrumKindCount] = {};
            for (const daw::MidiNoteEvent& n : result->song.tracks.front().notes)
                for (int k = 0; k < daw::kDrumKindCount; ++k)
                    if (n.key == daw::generalMidiDrumKey(static_cast<daw::DrumKind>(k)))
                        ++counts[k];
            QString message = tr("%1 drum notes from %2, on %3 beats (%4 BPM, %5 bars), %6:")
                                  .arg(result->notes)
                                  .arg(trackName)
                                  .arg(result->beats.beats.size())
                                  .arg(result->beats.bpm, 0, 'f', 1)
                                  .arg(result->beats.downbeats.size())
                                  .arg(grid > 0 ? tr("quantised to 1/%1 notes").arg(grid) : tr("as played"));
            message += QStringLiteral("\n") + tr("%1 kick, %2 snare, %3 closed hi-hat, %4 open hi-hat, %5 crash.")
                                                  .arg(counts[0]).arg(counts[1]).arg(counts[2]).arg(counts[3]).arg(counts[4]);
            message += QStringLiteral("\n\n") + tr("Saved as %1. The file changes tempo on every beat, so it stays on the song; "
                                                   "a DAW's bar lines fall on its bars.").arg(QDir::toNativeSeparators(path));
            const bool canPlay = !soundFont_.empty() && std::filesystem::exists(soundFont_);
            message += QStringLiteral("\n\n") + (canPlay ? tr("Added as a MIDI track, so you can hear it against the stem.")
                                                         : tr("Choose a SoundFont (Settings > SoundFont for MIDI) to hear it as a track."));
            daw::logging::info("midi", "drums to midi", {{"track", trackName.toStdString()}, {"notes", result->notes},
                                                         {"bpm", result->beats.bpm}, {"file", path.toStdString()}});
            if (canPlay)
                importAudio({path});
            Q_EMIT drumsToMidiFinished(true, message, path);
        });
}

void ProjectController::loadAllMedia()
{
    loadMediaItems(model_.media);
}

void ProjectController::setSoundFont(const QString& path)
{
    const fs::path next = toPath(path);
    if (next == soundFont_)
        return;
    soundFont_ = next;
    std::vector<daw::MediaRef> midi;
    for (const daw::MediaRef& media : model_.media)
        if (daw::isMidiFile(daw::utf8ToPath(media.path)))
            midi.push_back(media);
    if (!midi.empty()) {
        Q_EMIT statusMessage(tr("Rendering %n MIDI track(s) with the new SoundFont...", nullptr, static_cast<int>(midi.size())));
        markStemReportStale();
        loadMediaItems(midi);
    }
}

QString ProjectController::soundFont() const
{
    return toQString(soundFont_);
}

void ProjectController::loadMediaItems(const std::vector<daw::MediaRef>& items)
{
    const fs::path root = root_;
    const double rate = engineRate_;
    const fs::path soundFont = soundFont_;
    const auto token = cancel_->token();
    for (const daw::MediaRef& media : items) {
        runInBackground(
            [root, media, rate, soundFont, token] {
                daw::MediaLoadOptions options;
                options.engineSampleRate = rate;
                options.soundFont = soundFont;
                return daw::loadMedia(root, media, options, token);
            },
            [this, name = media.name](daw::Result<daw::LoadedMedia>& result) {
                if (!result) {
                    Q_EMIT statusMessage(tr("Could not load %1: %2")
                                             .arg(QString::fromStdString(name), QString::fromStdString(result.error().describe())));
                    return;
                }
                const auto id = result->id;
                pool_.put(std::move(*result));
                if (engineRate_ != 0.0 && pool_.get(id)->renditionRate != engineRate_)
                    retargetAllMedia();
                rebuildSnapshot();
                Q_EMIT mediaChanged();
                maybeRunStemReport();
            });
    }
}

void ProjectController::retargetAllMedia()
{
    const double rate = engineRate_;
    const auto token = cancel_->token();
    for (const daw::MediaId& id : pool_.ids()) {
        auto media = pool_.get(id);
        if (!media || media->renditionRate == rate)
            continue;
        runInBackground([media = *media, rate, token] { return daw::retargetMedia(media, rate, token); },
                        [this, rate](daw::Result<daw::LoadedMedia>& result) {
                            if (!result || rate != engineRate_)
                                return;
                            pool_.put(std::move(*result));
                            rebuildSnapshot();
                        });
    }
}

void ProjectController::onEngineRateChanged(double rate)
{
    if (rate == engineRate_)
        return;
    const std::int64_t position = playheadProjectFrames();
    engineRate_ = rate;
    if (rate <= 0.0) {
        model_.playhead = position;
        return;
    }
    pendingPlayhead_ = position;
    retargetAllMedia();
    rebuildSnapshot();
}

void ProjectController::rebuildSnapshot()
{
    if (engineRate_ <= 0.0)
        return;
    daw::SnapshotBuildReport report;
    engine_.engine().publishSnapshot(daw::buildPlaybackSnapshot(model_, pool_, engineRate_, ++snapshotVersion_, &report,
                                                                {.bypassCleanup = cleanupBypassed_, .listenRemoved = listenRemoved_}));
    if (pendingPlayhead_ >= 0) {
        engine_.seek(daw::TimeMapper{model_.sampleRate, engineRate_}.toEngine(pendingPlayhead_));
        pendingPlayhead_ = -1;
    }
}

std::int64_t ProjectController::playheadProjectFrames() const
{
    if (pendingPlayhead_ >= 0)
        return pendingPlayhead_;
    if (engineRate_ <= 0.0)
        return model_.playhead;
    return daw::TimeMapper{model_.sampleRate, engineRate_}.toProject(engine_.transport().playhead);
}

void ProjectController::seekProjectFrames(std::int64_t frame)
{
    frame = std::max<std::int64_t>(0, frame);
    if (engineRate_ <= 0.0) {
        model_.playhead = frame;
        pendingPlayhead_ = -1;
        return;
    }
    pendingPlayhead_ = -1;
    engine_.seek(daw::TimeMapper{model_.sampleRate, engineRate_}.toEngine(frame));
}

void ProjectController::setDirty(bool dirty)
{
    if (dirty_ == dirty)
        return;
    dirty_ = dirty;
    Q_EMIT dirtyChanged(dirty);
}

void ProjectController::autosave()
{
    if (!dirty_ || untitled_)
        return;
    daw::ProjectModel snapshot = model_;
    snapshot.playhead = playheadProjectFrames();
    if (auto status = daw::ProjectStore::writeAutosave(root_, snapshot); !status)
        daw::logging::warn("project", "autosave failed", {{"error", status.error().describe()}});
    else
        daw::logging::debug("project", "autosaved");
}

void ProjectController::analyzeAll()
{
    std::vector<std::string> analyzers{daw::kLevelAnalyzerId, daw::kTransientAnalyzerId, daw::kSpectralAnalyzerId};
    if (analysis_.inference() != nullptr)
        analyzers.push_back(daw::kInferenceProbeAnalyzerId);

    int submitted = 0;
    const std::uint64_t generation = generation_;
    for (const daw::MediaRef& media : model_.media) {
        const auto loaded = pool_.get(media.id);
        if (!loaded)
            continue;
        for (const std::string& analyzer : analyzers) {
            daw::JobRequest request{analyzer, loaded->original, nlohmann::json::object(), media.id.str()};
            auto job = analysis_.submit(std::move(request), [this, generation](const daw::JobOutcome& outcome) {
                QMetaObject::invokeMethod(
                    this,
                    [this, generation, outcome] {
                        if (generation == generation_)
                            handleAnalysis(outcome);
                    },
                    Qt::QueuedConnection);
            });
            if (job)
                ++submitted;
        }
    }
    Q_EMIT statusMessage(submitted > 0 ? tr("Analysing (%1 jobs in background)...").arg(submitted)
                                       : tr("Nothing to analyse: import audio first."));
}

void ProjectController::handleAnalysis(const daw::JobOutcome& outcome)
{
    const daw::MediaId mediaId(outcome.status.tag);
    const daw::MediaRef* media = model_.findMedia(mediaId);
    if (media == nullptr)
        return;
    const QString mediaName = QString::fromStdString(media->name);

    if (outcome.status.state != daw::JobState::Succeeded || !outcome.result) {
        if (outcome.status.state != daw::JobState::Cancelled)
            Q_EMIT analysisText(QStringLiteral("[%1] %2 failed: %3\n")
                                    .arg(mediaName, QString::fromStdString(outcome.status.analyzerId),
                                         QString::fromStdString(outcome.status.error ? outcome.status.error->describe() : "unknown")));
        return;
    }

    const daw::AnalysisResult& result = *outcome.result;
    const std::string fileName = result.analyzerId + "-v" + std::to_string(result.analyzerVersion) + ".json";
    const fs::path file = daw::ProjectStore::analysisDirectory(root_, mediaId) / daw::utf8ToPath(fileName);
    const nlohmann::json document = {{"analyzer", result.analyzerId},
                                     {"analyzerVersion", result.analyzerVersion},
                                     {"mediaId", mediaId.str()},
                                     {"createdUtc", daw::utcTimestampIso8601()},
                                     {"elapsedMs", result.elapsedMs},
                                     {"data", result.data}};
    if (auto status = daw::writeFileAtomically(file, document.dump(2)); !status) {
        Q_EMIT statusMessage(tr("Could not store analysis: %1").arg(QString::fromStdString(status.error().describe())));
        return;
    }

    const std::string relative = daw::pathToGenericUtf8(fs::relative(file, root_));
    auto existing = std::find_if(model_.analyses.begin(), model_.analyses.end(), [&](const daw::AnalysisRecord& r) {
        return r.media == mediaId && r.analyzer == result.analyzerId;
    });
    daw::AnalysisRecord record{existing != model_.analyses.end() ? existing->id : daw::AnalysisId::generate(), mediaId,
                               result.analyzerId, result.analyzerVersion, relative, daw::utcTimestampIso8601()};
    if (existing != model_.analyses.end())
        *existing = record;
    else
        model_.analyses.push_back(record);
    setDirty(true);

    Q_EMIT analysisText(summarise(result, mediaName));
    if (result.analyzerId == daw::kTransientAnalyzerId)
        reloadAnalysisMarkers();
}

void ProjectController::reloadAnalysisMarkers()
{
    markers_.clear();
    for (const daw::AnalysisRecord& record : model_.analyses) {
        if (record.analyzer != daw::kTransientAnalyzerId)
            continue;
        const daw::MediaRef* media = model_.findMedia(record.media);
        auto bytes = daw::readFileBytes(root_ / daw::utf8ToPath(record.resultPath));
        if (media == nullptr || !bytes)
            continue;
        const auto document = nlohmann::json::parse(*bytes, nullptr, false);
        if (document.is_discarded())
            continue;
        const auto frames = daw::onsetFrames(document.value("data", nlohmann::json::object()));
        for (const daw::Track& track : model_.tracks)
            for (const daw::Clip& clip : track.clips) {
                if (clip.media != record.media)
                    continue;
                for (std::int64_t frame : frames) {
                    const auto projectFrame = static_cast<std::int64_t>(std::llround(static_cast<double>(frame) * model_.sampleRate / media->sampleRate));
                    if (projectFrame >= clip.sourceStart && projectFrame < clip.sourceStart + clip.length)
                        markers_.push_back(clip.timelineStart + projectFrame - clip.sourceStart);
                }
            }
    }
    std::sort(markers_.begin(), markers_.end());
    Q_EMIT markersChanged();
}

bool ProjectController::allClipMediaLoaded() const
{
    for (const daw::Track& track : model_.tracks)
        for (const daw::Clip& clip : track.clips)
            if (!pool_.contains(clip.media))
                return false;
    return true;
}

void ProjectController::markStemReportStale()
{
    if (!stemReport_ || stemReportStale_)
        return;
    stemReportStale_ = true;
    Q_EMIT stemReportChanged();
}

void ProjectController::maybeRunStemReport()
{
    if (!stemReportWanted_ || !imports_.empty() || !allClipMediaLoaded())
        return;
    stemReportWanted_ = false;
    if (model_.contentEnd() > 0)
        runStemReport();
}

void ProjectController::runStemReport()
{
    if (model_.contentEnd() <= 0) {
        Q_EMIT statusMessage(tr("Nothing to analyse: import stems first."));
        return;
    }
    if (!imports_.empty() || !allClipMediaLoaded()) {
        stemReportWanted_ = true; // runs as soon as the stems are loaded
        Q_EMIT statusMessage(tr("The stem report will run when all stems have loaded."));
        return;
    }

    // A newer request replaces a running one.
    stemCancel_->cancel();
    stemCancel_ = std::make_shared<daw::CancellationSource>();
    const auto token = stemCancel_->token();
    const std::uint64_t run = ++stemReportRun_;
    const std::uint64_t generation = generation_;
    const daw::ProjectModel model = model_;
    stemReportRunning_ = true;
    Q_EMIT stemReportChanged();

    // Not counted as a busy job: the report never blocks saving or exporting.
    QPointer<ProjectController> self(this);
    workers_.start([this, self, run, generation, model, token] {
        daw::StemReportOptions options;
        options.progress = [this, self, run](double p) {
            QMetaObject::invokeMethod(
                this, [self, run, p] { if (self && run == self->stemReportRun_) Q_EMIT self->stemReportProgress(p); },
                Qt::QueuedConnection);
        };
        auto result = std::make_shared<daw::Result<daw::StemReport>>(daw::analyzeStems(model, pool_, options, token));
        QMetaObject::invokeMethod(
            this,
            [this, run, generation, result] {
                if (generation != generation_ || run != stemReportRun_)
                    return; // superseded
                stemReportRunning_ = false;
                if (!*result) {
                    if (result->error().code != daw::ErrorCode::Cancelled) {
                        daw::logging::warn("analysis", "stem report failed", {{"error", result->error().describe()}});
                        Q_EMIT stemReportFailed(QString::fromStdString(result->error().describe()));
                    }
                    Q_EMIT stemReportChanged();
                    return;
                }
                stemReport_ = std::move(**result);
                stemReportStale_ = false;
                const fs::path file = root_ / daw::utf8ToPath("Analysis") / daw::utf8ToPath("stem-report.json");
                if (auto status = daw::writeFileAtomically(file, daw::toJson(*stemReport_).dump(2)); !status)
                    daw::logging::warn("analysis", "could not store the stem report", {{"error", status.error().describe()}});
                daw::logging::info("analysis", "stem report",
                                   {{"tracks", stemReport_->tracks.size()},
                                    {"advice", stemReport_->advice.size()},
                                    {"mixLufs", stemReport_->mix.integratedLufs},
                                    {"mixTruePeak", stemReport_->mix.truePeakDbtp},
                                    {"seconds", stemReport_->analysisSeconds}});
                Q_EMIT stemReportChanged();
            },
            Qt::QueuedConnection);
    });
}
