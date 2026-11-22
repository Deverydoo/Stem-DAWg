// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "ProjectController.h"

#include "EngineBridge.h"

#include <daw/ai/Analyzers.h>
#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/project/ProjectStore.h>
#include <daw/session/MediaLoader.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/SnapshotBuilder.h>

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <cmath>
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
};

ProjectController::ProjectController(EngineBridge& engine, daw::AnalysisService& analysis, QObject* parent)
    : QObject(parent), engine_(engine), analysis_(analysis), cancel_(std::make_shared<daw::CancellationSource>())
{
    workers_.setMaxThreadCount(2);
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
    analysis_.cancelAll();
    analysis_.waitIdle(std::chrono::seconds(10)); // callbacks target this object
    workers_.clear();
    workers_.waitForDone();
    if (untitled_) {
        std::error_code ec;
        fs::remove_all(root_, ec);
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
    ++generation_;
    analysis_.cancelAll();
    engine_.stop();
    pool_.clear();
    markers_.clear();
    imports_.clear();
    pendingPlayhead_ = -1;
    if (untitled_ && !root_.empty()) {
        std::error_code ec;
        fs::remove_all(root_, ec);
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
    setDirty(recoverAutosave);
    pendingPlayhead_ = model_.playhead;
    rebuildSnapshot();
    Q_EMIT projectChanged();
    Q_EMIT analysisText({});
    reloadAnalysisMarkers();
    loadAllMedia();
    Q_EMIT statusMessage(tr("Opened %1").arg(directory));
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

    const fs::path root = root_;
    const double rate = engineRate_;
    const auto token = cancel_->token();
    for (const QString& file : ordered) {
        const std::uint64_t ticket = nextImportTicket_++;
        imports_.push_back(PendingImport{ticket, file, nullptr});
        const fs::path source = toPath(file);
        runInBackground(
            [root, source, rate, token] {
                auto out = std::make_shared<ImportResult>();
                out->ref = daw::importAudioFile(root, source);
                if (out->ref) {
                    daw::MediaLoadOptions options;
                    options.engineSampleRate = rate;
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
        applyImport(next.file, *next.result);
    }
}

void ProjectController::applyImport(const QString& file, ImportResult& result)
{
    if (!result.ref || !result.loaded) {
        const auto& err = !result.ref ? result.ref.error() : result.loaded.error();
        Q_EMIT statusMessage(tr("Import failed for %1: %2").arg(file, QString::fromStdString(err.describe())));
        daw::logging::warn("project", "import failed", {{"file", file.toStdString()}, {"error", err.describe()}});
        return;
    }
    daw::addMediaAsNewTrack(model_, *result.ref);
    pool_.put(std::move(*result.loaded));
    setDirty(true);
    if (engineRate_ != 0.0 && pool_.get(result.ref->id)->renditionRate != engineRate_)
        retargetAllMedia(); // device changed while importing
    rebuildSnapshot();
    Q_EMIT projectChanged();
    Q_EMIT mediaChanged();
    Q_EMIT statusMessage(tr("Imported %1").arg(QFileInfo(file).fileName()));
}

template <typename Mutate>
void ProjectController::mutateTrack(const daw::TrackId& id, Mutate mutate)
{
    daw::Track* track = model_.findTrack(id);
    if (track == nullptr || !mutate(*track))
        return;
    setDirty(true);
    rebuildSnapshot();
    Q_EMIT mixerChanged();
}

void ProjectController::setTrackGain(const daw::TrackId& id, double gainDb)
{
    gainDb = std::clamp(gainDb, daw::kSilenceDb, 12.0);
    mutateTrack(id, [gainDb](daw::Track& t) { return std::exchange(t.gainDb, gainDb) != gainDb; });
}

void ProjectController::setTrackPan(const daw::TrackId& id, double pan)
{
    pan = std::clamp(pan, -1.0, 1.0);
    mutateTrack(id, [pan](daw::Track& t) { return std::exchange(t.pan, pan) != pan; });
}

void ProjectController::setTrackMuted(const daw::TrackId& id, bool muted)
{
    mutateTrack(id, [muted](daw::Track& t) { return std::exchange(t.muted, muted) != muted; });
}

void ProjectController::setTrackSoloed(const daw::TrackId& id, bool soloed)
{
    mutateTrack(id, [soloed](daw::Track& t) { return std::exchange(t.soloed, soloed) != soloed; });
}

void ProjectController::setMasterGain(double gainDb)
{
    gainDb = std::clamp(gainDb, daw::kSilenceDb, 12.0);
    if (model_.masterGainDb == gainDb)
        return;
    model_.masterGainDb = gainDb;
    setDirty(true);
    rebuildSnapshot();
    Q_EMIT mixerChanged();
}

void ProjectController::setProfileId(const std::string& id, bool markDirty)
{
    if (model_.profileId == id)
        return;
    model_.profileId = id;
    if (markDirty)
        setDirty(true);
}

void ProjectController::setReleaseMetadata(const daw::MetadataFields& fields)
{
    if (model_.release == fields)
        return;
    model_.release = fields;
    setDirty(true);
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
                                {"clipped", r.render.clippedSamples},
                                {"renderSeconds", r.render.renderSeconds}});
            Q_EMIT exportFinished(true, r.render.clippedSamples > 0 || !r.artworkWarning.empty(), message, file);
        });
}

void ProjectController::loadAllMedia()
{
    const fs::path root = root_;
    const double rate = engineRate_;
    const auto token = cancel_->token();
    for (const daw::MediaRef& media : model_.media) {
        runInBackground(
            [root, media, rate, token] {
                daw::MediaLoadOptions options;
                options.engineSampleRate = rate;
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
    engine_.engine().publishSnapshot(daw::buildPlaybackSnapshot(model_, pool_, engineRate_, ++snapshotVersion_, &report));
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
