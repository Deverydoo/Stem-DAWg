// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "MainWindow.h"

#include "AboutDialog.h"
#include "DeviceDialog.h"
#include "DiagnosticsPanel.h"
#include "EngineBridge.h"
#include "ExportDialog.h"
#include "HelpText.h"
#include "ProfileDialog.h"
#include "ProjectController.h"
#include "TrackPanel.h"
#include "TrackStyle.h"
#include "WaveformView.h"

#include <daw/ai/AnalysisService.h>
#include <daw/ai/Analyzers.h>
#include <daw/ai/BuiltinModels.h>
#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/media/AudioFileDecoder.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QProcess>
#include <QPushButton>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

#include <cmath>
#include <utility>

namespace {

QString formatPosition(std::int64_t frames, double rate, double bpm, int beatsPerBar)
{
    const double seconds = rate > 0.0 ? static_cast<double>(frames) / rate : 0.0;
    const int minutes = static_cast<int>(seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    const double beats = seconds * bpm / 60.0;
    const auto bar = static_cast<long long>(beats / beatsPerBar) + 1;
    const auto beat = static_cast<long long>(std::fmod(beats, beatsPerBar)) + 1;
    const auto ticks = static_cast<int>((beats - std::floor(beats)) * 960.0);
    return QStringLiteral("%1:%2   %3.%4.%5")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(rest, 6, 'f', 3, QLatin1Char('0'))
        .arg(bar)
        .arg(beat)
        .arg(ticks, 3, 10, QLatin1Char('0'));
}

enum class Glyph { Play, Pause, Stop, ToStart };

QIcon transportIcon(Glyph glyph)
{
    QIcon icon;
    const std::pair<QIcon::Mode, QColor> modes[] = {{QIcon::Normal, QColor(0xe6, 0xe6, 0xe6)},
                                                    {QIcon::Active, QColor(0xff, 0xff, 0xff)},
                                                    {QIcon::Disabled, QColor(0x70, 0x70, 0x70)}};
    for (const auto& [mode, colour] : modes) {
        QPixmap pixmap(64, 64);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        QPainterPath path;
        switch (glyph) {
        case Glyph::Play:
            path.moveTo(18, 12);
            path.lineTo(52, 32);
            path.lineTo(18, 52);
            path.closeSubpath();
            p.drawPath(path);
            break;
        case Glyph::Pause:
            p.drawRoundedRect(QRectF(16, 12, 12, 40), 2, 2);
            p.drawRoundedRect(QRectF(36, 12, 12, 40), 2, 2);
            break;
        case Glyph::Stop:
            p.drawRoundedRect(QRectF(15, 15, 34, 34), 3, 3);
            break;
        case Glyph::ToStart:
            p.drawRect(QRectF(14, 14, 7, 36));
            path.moveTo(50, 14);
            path.lineTo(23, 32);
            path.lineTo(50, 50);
            path.closeSubpath();
            p.drawPath(path);
            break;
        }
        p.end();
        icon.addPixmap(pixmap, mode);
    }
    return icon;
}

QString audioFileFilter()
{
    QStringList patterns;
    for (const auto& ext : daw::supportedAudioExtensions())
        patterns << QStringLiteral("*") + QString::fromStdString(ext);
    return QObject::tr("Audio files (%1)").arg(patterns.join(' '));
}

} // namespace

MainWindow::MainWindow()
{
    setAcceptDrops(true);
    engine_ = std::make_unique<EngineBridge>();
    buildUi();

    // Route structured log records into the diagnostics panel (queued: the
    // logger calls sinks on its writer thread).
    DiagnosticsPanel* panel = diagnostics_;
    logSink_ = daw::Logger::instance().addSink([panel](const daw::LogRecord& record) {
        QString line = QStringLiteral("%1 %2 [%3] %4")
                           .arg(QString::fromStdString(record.timestamp.substr(11, 12)),
                                QString::fromLatin1(daw::toString(record.level)).toUpper(),
                                QString::fromStdString(record.category), QString::fromStdString(record.message));
        if (record.fields.is_object() && !record.fields.empty())
            line += QStringLiteral(" ") + QString::fromStdString(record.fields.dump());
        QMetaObject::invokeMethod(panel, [panel, line] { panel->appendLog(line); }, Qt::QueuedConnection);
    });

    startAi();
    if (auto loaded = daw::loadProfileLibrary(daw::defaultProfileLibraryPath()))
        profiles_ = std::move(*loaded);
    else
        daw::logging::warn("ui", "could not read artist profiles", {{"error", loaded.error().describe()}});
    project_ = std::make_unique<ProjectController>(*engine_, *analysis_);
    project_->setDefaultProfileId(profiles_.activeProfileId);
    project_->setProfileId(profiles_.activeProfileId, false);
    buildMenus();
    connectSignals();
    startAudio();
    refreshLanes();
    refreshProfileBox();
    updateTitle();
}

MainWindow::~MainWindow()
{
    daw::Logger::instance().removeSink(logSink_);
    project_.reset();  // waits for its background jobs and analysis callbacks
    analysis_.reset(); // joins analysis workers
    engine_.reset();   // closes the device, then the engine
}

void MainWindow::buildUi()
{
    waveform_ = new WaveformView(this);
    trackPanel_ = new TrackPanel(WaveformView::kRulerHeight, this);
    trackPanel_->setFixedWidth(250);
    laneScroll_ = new QScrollBar(Qt::Vertical, this);
    laneScroll_->setVisible(false);

    auto* arrangement = new QWidget(this);
    auto* arrangementLayout = new QHBoxLayout(arrangement);
    arrangementLayout->setContentsMargins(0, 0, 0, 0);
    arrangementLayout->setSpacing(0);
    arrangementLayout->addWidget(trackPanel_);
    arrangementLayout->addWidget(waveform_, 1);
    arrangementLayout->addWidget(laneScroll_);
    setCentralWidget(arrangement);

    auto* transport = addToolBar(tr("Transport"));
    transport->setObjectName(QStringLiteral("transport"));
    transport->setMovable(false);
    transport->setIconSize(QSize(20, 20));

    playIcon_ = transportIcon(Glyph::Play);
    pauseIcon_ = transportIcon(Glyph::Pause);

    auto* home = transport->addAction(transportIcon(Glyph::ToStart), tr("Return to Start"));
    home->setToolTip(helpTip(tr("Return to start (Home)"), tr("Moves the playhead to the beginning without stopping playback.")));
    home->setShortcut(Qt::Key_Home);
    home->setShortcutContext(Qt::ApplicationShortcut);
    connect(home, &QAction::triggered, this, [this] { project_->seekProjectFrames(0); });

    playAction_ = transport->addAction(playIcon_, tr("Play"));
    playAction_->setShortcut(Qt::Key_Space);
    playAction_->setShortcutContext(Qt::ApplicationShortcut);
    playAction_->setToolTip(helpTip(tr("Play / Pause (Space)"), tr("Starts playback, or pauses and keeps the playhead where it is.")));
    connect(playAction_, &QAction::triggered, this, &MainWindow::togglePlayPause);

    auto* stop = transport->addAction(transportIcon(Glyph::Stop), tr("Stop"));
    stop->setToolTip(helpTip(tr("Stop"), tr("Stops playback and returns to the beginning of the song.")));
    connect(stop, &QAction::triggered, this, &MainWindow::stopAndReturnToStart);

    for (QAction* action : {home, playAction_, stop})
        if (auto* button = qobject_cast<QToolButton*>(transport->widgetForAction(action))) {
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setMinimumSize(34, 30);
        }

    transport->addSeparator();
    timeLabel_ = new QLabel(this);
    timeLabel_->setMinimumWidth(260);
    timeLabel_->setStyleSheet(QStringLiteral("font: 15pt 'Consolas'; color: #ff8a3d; padding: 0 12px;"));
    timeLabel_->setToolTip(helpTip(tr("Position"), tr("Left: minutes:seconds.milliseconds.\nRight: bar.beat.tick at the project tempo (960 ticks per beat).")));
    transport->addWidget(timeLabel_);

    auto* fit = transport->addAction(tr("Zoom to fit"));
    fit->setToolTip(helpTip(tr("Zoom to fit (Ctrl+0)"), tr("Shows the whole song across the window. Ctrl+wheel over the waveforms zooms in and out.")));
    connect(fit, &QAction::triggered, waveform_, &WaveformView::zoomToFit);

    auto* analyze = transport->addAction(tr("Analyze"));
    analyze->setToolTip(helpTip(tr("Analyze (Ctrl+Shift+A)"),
                                tr("Measures every stem in the background: levels and clipping, hits (yellow markers), frequency balance.\n"
                                   "Analysis never changes the audio. Results appear in the Analysis panel.")));

    transport->addSeparator();
    auto* artistLabel = new QLabel(tr(" Artist "), this);
    profileBox_ = new QComboBox(this);
    profileBox_->setMinimumWidth(170);
    const QString profileTip = helpTip(tr("Artist profile"), tr("Whose metadata (artist, label, copyright, artwork...) is written into exports of this project.\n"
                                                                 "Edit profiles in Settings > Artist Profiles."));
    artistLabel->setToolTip(profileTip);
    profileBox_->setToolTip(profileTip);
    transport->addWidget(artistLabel);
    transport->addWidget(profileBox_);
    connect(profileBox_, &QComboBox::activated, this, [this] {
        const std::string id = profileBox_->currentData().toString().toStdString();
        if (id == "__edit__") {
            refreshProfileBox();
            editProfiles(project_->model().profileId);
            return;
        }
        project_->setProfileId(id);
        profiles_.activeProfileId = id;
        project_->setDefaultProfileId(id);
        saveProfiles();
    });

    transport->addSeparator();
    auto* masterName = new QLabel(tr(" Master "), this);
    masterFader_ = new VolumeFader(this);
    masterFader_->setFixedWidth(130);
    masterLabel_ = new QLabel(VolumeFader::formatDb(0.0), this);
    masterLabel_->setFixedWidth(56);
    masterLabel_->setStyleSheet(QStringLiteral("color: #9a9da5; font: 8pt 'Consolas';"));
    const QString masterTip = helpTip(tr("Master volume"), tr("Overall level of the mix, after all tracks. Applies to playback and exports.\n"
                                                              "Stems from Suno often add up louder than 0 dBFS: lower this until the CLIP light stays off.\n"
                                                              "Double-click resets to 0 dB."));
    masterName->setToolTip(masterTip);
    masterFader_->setToolTip(masterTip);
    masterLabel_->setToolTip(masterTip);
    clipLight_ = new QToolButton(this);
    clipLight_->setText(tr("CLIP"));
    clipLight_->setFocusPolicy(Qt::NoFocus);
    clipLight_->setToolTip(helpTip(tr("Clip indicator"), tr("Lights red when the output went over 0 dBFS (distortion). Click to reset.")));
    transport->addWidget(masterName);
    transport->addWidget(masterFader_);
    transport->addWidget(masterLabel_);
    transport->addWidget(clipLight_);
    setClipIndicator(false);
    connect(clipLight_, &QToolButton::clicked, this, [this] { setClipIndicator(false); });
    connect(masterFader_, &VolumeFader::gainDbChanged, this, [this](double db) {
        masterLabel_->setText(VolumeFader::formatDb(db));
        project_->setMasterGain(db);
    });
    connect(analyze, &QAction::triggered, this, [this] { project_->analyzeAll(); });

    diagnostics_ = new DiagnosticsPanel(this);
    auto* diagDock = new QDockWidget(tr("Diagnostics"), this);
    diagDock->setObjectName(QStringLiteral("diagnostics"));
    diagDock->setWidget(diagnostics_);
    addDockWidget(Qt::RightDockWidgetArea, diagDock);

    analysisView_ = new QPlainTextEdit(this);
    analysisView_->setReadOnly(true);
    analysisView_->setFont(QFont(QStringLiteral("Consolas"), 9));
    auto* analysisDock = new QDockWidget(tr("Analysis"), this);
    analysisDock->setObjectName(QStringLiteral("analysis"));
    analysisDock->setWidget(analysisView_);
    addDockWidget(Qt::BottomDockWidgetArea, analysisDock);

    busyLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(busyLabel_);
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Project"), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(tr("&Open Project..."), QKeySequence::Open, this, &MainWindow::openProject);
    file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveProject);
    file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveProjectAs);
    file->addSeparator();
    file->addAction(tr("&Import Audio..."), QKeySequence(Qt::CTRL | Qt::Key_I), this, &MainWindow::importAudio)
        ->setToolTip(tr("Add WAV, FLAC, AIFF, OGG or MP3 stems as new tracks. They are copied into the project."));
    file->addAction(tr("&Export Mix..."), QKeySequence(Qt::CTRL | Qt::Key_E), this, &MainWindow::exportMixDialog)
        ->setToolTip(tr("Render the whole song to a WAV file with your artist profile's metadata."));
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    file->setToolTipsVisible(true);

    auto* audio = menuBar()->addMenu(tr("&Audio"));
    audio->addAction(tr("&Device Settings..."), this, &MainWindow::showDeviceDialog)
        ->setToolTip(tr("Choose the driver, output device, sample rate and buffer size."));
    audio->setToolTipsVisible(true);

    auto* settings = menuBar()->addMenu(tr("&Settings"));
    settings->addAction(tr("Artist &Profiles..."), this, [this] { editProfiles(project_->model().profileId); })
        ->setToolTip(tr("Enter your artist details once; they are written into every export."));
    settings->setToolTipsVisible(true);

    auto* analysis = menuBar()->addMenu(tr("A&nalysis"));
    analysis->addAction(tr("&Analyze All Media"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), this,
                        [this] { project_->analyzeAll(); })
        ->setToolTip(tr("Measure levels, hits and frequency balance of every stem. Does not change the audio."));
    analysis->setToolTipsVisible(true);

    auto* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Zoom to &Fit"), QKeySequence(Qt::CTRL | Qt::Key_0), waveform_, &WaveformView::zoomToFit);
    view->setToolTipsVisible(true);
    auto* split = view->addAction(tr("&Split Stereo Channels"));
    split->setToolTip(tr("On: one waveform per channel (left above right). Off: one combined waveform per track."));
    split->setCheckable(true);
    split->setChecked(QSettings().value(QStringLiteral("view/splitChannels"), true).toBool());
    waveform_->setSplitChannels(split->isChecked());
    connect(split, &QAction::toggled, this, [this](bool on) {
        waveform_->setSplitChannels(on);
        QSettings().setValue(QStringLiteral("view/splitChannels"), on);
    });

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->setToolTipsVisible(true);
    help->addAction(tr("&Source Code and Issues"), this,
                    [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Deverydoo/Stem-DAWg"))); })
        ->setToolTip(tr("Open the project on GitHub: report bugs, request features, contribute."));
    help->addSeparator();
    help->addAction(tr("&About Stem DAWg"), this, [this] { AboutDialog(aiStatus_, this).exec(); })
        ->setToolTip(tr("Version, licence and the open-source components Stem DAWg is built with."));
}

void MainWindow::connectSignals()
{
    connect(engine_.get(), &EngineBridge::tick, this, &MainWindow::onTick);
    connect(waveform_, &WaveformView::seekRequested, this, [this](qint64 frame) { project_->seekProjectFrames(frame); });

    connect(project_.get(), &ProjectController::projectChanged, this, [this] {
        refreshLanes();
        refreshProfileBox();
        updateTitle();
    });
    connect(project_.get(), &ProjectController::exportProgress, this,
            [this](double p) { busyLabel_->setText(tr("Rendering %1%").arg(static_cast<int>(p * 100.0))); });
    connect(project_.get(), &ProjectController::exportFinished, this, [this](bool ok, bool warning, const QString& message, const QString& path) {
        busyLabel_->setText(project_->isBusy() ? tr("Working...") : QString());
        if (!ok) {
            QMessageBox::warning(this, tr("Export failed"), message);
            return;
        }
        statusBar()->showMessage(tr("Exported %1").arg(path), 8000);
        QMessageBox box(warning ? QMessageBox::Warning : QMessageBox::Information, tr("Export finished"), message, QMessageBox::Ok, this);
        auto* show = box.addButton(tr("Show in Folder"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == show)
            QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
    });
    connect(project_.get(), &ProjectController::mediaChanged, this, &MainWindow::refreshLanes);
    connect(project_.get(), &ProjectController::mixerChanged, this, &MainWindow::refreshMixer);

    connect(waveform_, &WaveformView::laneLayoutChanged, this, [this](int laneHeight, int scrollY) {
        trackPanel_->setLaneLayout(laneHeight, scrollY);
        const QSignalBlocker block(laneScroll_);
        laneScroll_->setValue(scrollY);
    });
    connect(waveform_, &WaveformView::verticalRangeChanged, this, [this](int maximum, int pageStep) {
        laneScroll_->setRange(0, maximum);
        laneScroll_->setPageStep(pageStep);
        laneScroll_->setSingleStep(std::max(1, waveform_->laneHeight() / 2));
        laneScroll_->setVisible(maximum > 0);
    });
    connect(laneScroll_, &QScrollBar::valueChanged, waveform_, &WaveformView::setScrollY);
    connect(trackPanel_, &TrackPanel::scrollRequested, waveform_, &WaveformView::scrollByWheel);

    connect(trackPanel_, &TrackPanel::gainChanged, this,
            [this](const QString& id, double db) { project_->setTrackGain(daw::TrackId(id.toStdString()), db); });
    connect(trackPanel_, &TrackPanel::panChanged, this,
            [this](const QString& id, double pan) { project_->setTrackPan(daw::TrackId(id.toStdString()), pan); });
    connect(trackPanel_, &TrackPanel::muteToggled, this,
            [this](const QString& id, bool on) { project_->setTrackMuted(daw::TrackId(id.toStdString()), on); });
    connect(trackPanel_, &TrackPanel::soloToggled, this,
            [this](const QString& id, bool on) { project_->setTrackSoloed(daw::TrackId(id.toStdString()), on); });
    connect(project_.get(), &ProjectController::dirtyChanged, this, &MainWindow::updateTitle);
    connect(project_.get(), &ProjectController::statusMessage, this, [this](const QString& text) {
        statusBar()->showMessage(text, 6000);
        daw::logging::info("ui", text.toStdString());
    });
    connect(project_.get(), &ProjectController::busyChanged, this,
            [this](bool busy) { busyLabel_->setText(busy ? tr("Working...") : QString()); });
    connect(project_.get(), &ProjectController::analysisText, this, [this](const QString& text) {
        if (text.isEmpty())
            analysisView_->clear();
        else
            analysisView_->appendPlainText(text);
    });
    connect(project_.get(), &ProjectController::markersChanged, this,
            [this] { waveform_->setMarkers(project_->markers()); });
}

void MainWindow::startAi()
{
    std::shared_ptr<daw::InferenceBackend> backend;
    QString status;
    if (auto created = daw::createOnnxBackend()) {
        backend = *created;
        if (auto report = daw::runInferenceSelfTest(*backend)) {
            status = tr("%1, provider %2, self-test passed (%3 ms)")
                         .arg(QString::fromStdString(report->backendName), QString::fromLatin1(daw::toString(report->provider)))
                         .arg(report->latencyMs, 0, 'f', 2);
            daw::logging::info("ai", "inference self-test passed",
                               {{"backend", report->backendName}, {"latencyMs", report->latencyMs}});
        } else {
            status = tr("self-test FAILED: %1").arg(QString::fromStdString(report.error().describe()));
            daw::logging::error("ai", "inference self-test failed", {{"error", report.error().describe()}});
        }
    } else {
        status = tr("inference unavailable: %1").arg(QString::fromStdString(created.error().describe()));
        daw::logging::warn("ai", "inference unavailable", {{"error", created.error().describe()}});
    }

    aiStatus_ = tr("AI: %1").arg(status);
    analysis_ = std::make_unique<daw::AnalysisService>(daw::AnalysisServiceConfig{}, backend);
    (void)daw::registerBuiltinAnalyzers(*analysis_);
    diagnostics_->setAiStatus(tr("%1\n%2 workers, analyzers: %3")
                                  .arg(status)
                                  .arg(analysis_->workerCount())
                                  .arg(QString::fromStdString([this] {
                                      std::string ids;
                                      for (const auto& id : analysis_->analyzerIds())
                                          ids += (ids.empty() ? "" : ", ") + id;
                                      return ids;
                                  }())));
}

void MainWindow::startAudio()
{
    QSettings settings;
    daw::AudioDeviceSetup setup;
    setup.typeName = settings.value(QStringLiteral("audio/type")).toString().toStdString();
    setup.outputDeviceName = settings.value(QStringLiteral("audio/device")).toString().toStdString();
    setup.sampleRate = settings.value(QStringLiteral("audio/sampleRate"), 0.0).toDouble();
    setup.bufferSize = settings.value(QStringLiteral("audio/bufferSize"), 0).toInt();

    auto status = engine_->openDevice(setup);
    if (!status && setup != daw::AudioDeviceSetup{}) {
        daw::logging::warn("audio", "saved device failed; trying default", {{"error", status.error().describe()}});
        status = engine_->openDevice({});
    }
    if (!status) {
        statusBar()->showMessage(tr("No audio device: %1").arg(QString::fromStdString(status.error().describe())));
        daw::logging::error("audio", "no audio device", {{"error", status.error().describe()}});
    }
}

void MainWindow::onTick()
{
    const auto transport = engine_->transport();
    if (transport.playing != shownPlaying_) {
        shownPlaying_ = transport.playing;
        playAction_->setIcon(transport.playing ? pauseIcon_ : playIcon_);
        playAction_->setText(transport.playing ? tr("Pause") : tr("Play"));
    }

    const auto& model = project_->model();
    const std::int64_t playhead = project_->playheadProjectFrames();
    waveform_->setPlayhead(playhead);
    timeLabel_->setText(formatPosition(playhead, model.sampleRate, model.tempo.bpm, model.tempo.numerator));

    const auto peaks = engine_->engine().takeMeterPeaks();
    if (peaks.left >= 1.0f || peaks.right >= 1.0f)
        setClipIndicator(true);
    if (diagnostics_->isVisible())
        diagnostics_->updateEngine(engine_->engine().diagnostics(), engine_->devices().status(), peaks);
}

void MainWindow::refreshLanes()
{
    const auto& model = project_->model();
    std::vector<WaveformLane> lanes;
    for (std::size_t i = 0; i < model.tracks.size(); ++i) {
        const auto& track = model.tracks[i];
        WaveformLane lane;
        lane.name = QString::fromStdString(track.name);
        lane.color = trackColor(lane.name, static_cast<int>(i));
        lane.audible = model.isAudible(track);
        for (const auto& clip : track.clips) {
            WaveformClip view;
            view.timelineStart = clip.timelineStart;
            view.sourceStart = clip.sourceStart;
            view.length = clip.length;
            view.name = QString::fromStdString(clip.name);
            if (const auto* media = model.findMedia(clip.media))
                view.mediaRate = media->sampleRate;
            if (auto loaded = project_->media().get(clip.media))
                view.peaks = loaded->peaks;
            lane.clips.push_back(std::move(view));
        }
        lanes.push_back(std::move(lane));
    }
    waveform_->setTimeline(model.sampleRate, model.tempo.bpm, model.tempo.numerator);
    waveform_->setLanes(std::move(lanes));
    trackPanel_->setTopText(
        tr("%1 BPM   %2/%3").arg(model.tempo.bpm, 0, 'f', 1).arg(model.tempo.numerator).arg(model.tempo.denominator));
    refreshMixer();
}

void MainWindow::refreshMixer()
{
    const auto& model = project_->model();
    std::vector<TrackHeaderState> headers;
    std::vector<bool> audible;
    headers.reserve(model.tracks.size());
    for (std::size_t i = 0; i < model.tracks.size(); ++i) {
        const auto& track = model.tracks[i];
        TrackHeaderState state;
        state.id = QString::fromStdString(track.id.str());
        state.name = QString::fromStdString(track.name);
        state.number = static_cast<int>(i) + 1;
        state.color = trackColor(state.name, static_cast<int>(i));
        state.gainDb = track.gainDb;
        state.pan = track.pan;
        state.muted = track.muted;
        state.soloed = track.soloed;
        state.audible = model.isAudible(track);
        audible.push_back(state.audible);
        headers.push_back(std::move(state));
    }
    trackPanel_->setTracks(headers);
    waveform_->setLaneAudibility(audible);
    if (masterFader_->gainDb() != model.masterGainDb && !masterFader_->isSliderDown())
        masterFader_->setGainDb(model.masterGainDb);
    masterLabel_->setText(VolumeFader::formatDb(model.masterGainDb));
}

void MainWindow::togglePlayPause()
{
    if (engine_->transport().playing) {
        engine_->stop(); // pause: the playhead stays where it is
        return;
    }
    // Starting from the end of the content would stop immediately: restart.
    const std::int64_t end = project_->model().contentEnd();
    if (end > 0 && project_->playheadProjectFrames() >= end)
        project_->seekProjectFrames(0);
    engine_->play();
}

void MainWindow::stopAndReturnToStart()
{
    engine_->stop();
    project_->seekProjectFrames(0);
}

void MainWindow::updateTitle()
{
    const QString location = project_->isUntitled() ? tr("unsaved") : QString::fromStdString(daw::pathToUtf8(project_->root()));
    setWindowTitle(QStringLiteral("%1%2 - %3 - Stem DAWg")
                       .arg(project_->displayName(), project_->isDirty() ? QStringLiteral(" *") : QString(), location));
}

bool MainWindow::confirmDiscardChanges()
{
    if (!project_->isDirty() && !(project_->isUntitled() && !project_->model().media.empty()))
        return true;
    const auto answer = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to \"%1\"?").arg(project_->displayName()),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
        return saveProject();
    return true;
}

void MainWindow::newProject()
{
    if (confirmDiscardChanges())
        project_->newProject();
}

void MainWindow::openProject()
{
    if (!confirmDiscardChanges())
        return;
    QSettings settings;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Project Folder"),
                                                          settings.value(QStringLiteral("project/lastDir")).toString());
    if (dir.isEmpty())
        return;
    bool recover = false;
    if (ProjectController::hasRecoverableAutosave(dir)) {
        recover = QMessageBox::question(this, tr("Recover autosave"),
                                        tr("This project has autosaved changes newer than the last save. Recover them?"))
                  == QMessageBox::Yes;
    }
    QString error;
    if (!project_->openProject(dir, recover, &error)) {
        QMessageBox::warning(this, tr("Open failed"), error);
        return;
    }
    settings.setValue(QStringLiteral("project/lastDir"), dir);
}

bool MainWindow::saveProject()
{
    if (project_->isUntitled())
        return saveProjectAs();
    QString error;
    if (!project_->save(&error)) {
        QMessageBox::warning(this, tr("Save failed"), error);
        return false;
    }
    return true;
}

bool MainWindow::saveProjectAs()
{
    QSettings settings;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose or Create an Empty Project Folder"),
                                                          settings.value(QStringLiteral("project/lastDir")).toString());
    if (dir.isEmpty())
        return false;
    QString error;
    if (!project_->saveAs(dir, &error)) {
        QMessageBox::warning(this, tr("Save failed"), error);
        return false;
    }
    settings.setValue(QStringLiteral("project/lastDir"), dir);
    return true;
}

void MainWindow::importAudio()
{
    QSettings settings;
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Audio"),
                                                            settings.value(QStringLiteral("import/lastDir")).toString(),
                                                            audioFileFilter());
    if (files.isEmpty())
        return;
    settings.setValue(QStringLiteral("import/lastDir"), QFileInfo(files.front()).absolutePath());
    project_->importAudio(files);
}

void MainWindow::showDeviceDialog()
{
    DeviceDialog dialog(engine_->devices(), engine_->devices().currentSetup(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto setup = dialog.selectedSetup();
    if (auto status = engine_->openDevice(setup); !status) {
        QMessageBox::warning(this, tr("Audio device"), QString::fromStdString(status.error().describe()));
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("audio/type"), QString::fromStdString(setup.typeName));
    settings.setValue(QStringLiteral("audio/device"), QString::fromStdString(setup.outputDeviceName));
    settings.setValue(QStringLiteral("audio/sampleRate"), setup.sampleRate);
    settings.setValue(QStringLiteral("audio/bufferSize"), setup.bufferSize);
}

void MainWindow::setClipIndicator(bool clipped)
{
    clipped_ = clipped;
    clipLight_->setStyleSheet(clipped ? QStringLiteral("QToolButton { background: #d23a2a; color: white; font-weight: bold; border-radius: 3px; padding: 2px 6px; }")
                                      : QStringLiteral("QToolButton { background: #34373d; color: #6c6f76; font-weight: bold; border-radius: 3px; padding: 2px 6px; }"));
}

void MainWindow::saveProfiles()
{
    if (auto status = daw::saveProfileLibrary(daw::defaultProfileLibraryPath(), profiles_); !status)
        QMessageBox::warning(this, tr("Artist profiles"), tr("Could not save profiles: %1").arg(QString::fromStdString(status.error().describe())));
}

void MainWindow::refreshProfileBox()
{
    const QSignalBlocker block(profileBox_);
    profileBox_->clear();
    profileBox_->addItem(tr("(no profile)"), QString());
    const std::string& current = project_->model().profileId;
    int select = 0;
    for (const auto& p : profiles_.profiles) {
        profileBox_->addItem(QString::fromStdString(p.name), QString::fromStdString(p.id));
        if (p.id == current)
            select = profileBox_->count() - 1;
    }
    if (!current.empty() && select == 0) {
        profileBox_->addItem(tr("(profile not on this PC)"), QString::fromStdString(current));
        select = profileBox_->count() - 1;
    }
    profileBox_->insertSeparator(profileBox_->count());
    profileBox_->addItem(tr("Edit Profiles..."), QStringLiteral("__edit__"));
    profileBox_->setCurrentIndex(select);
}

std::optional<daw::ProfileLibrary> MainWindow::editProfiles(const std::string& selectId)
{
    ProfileDialog dialog(profiles_, selectId, this);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    profiles_ = dialog.library();
    saveProfiles();
    // A project without a profile adopts the first one created.
    if (project_->model().profileId.empty() && !profiles_.activeProfileId.empty())
        project_->setProfileId(profiles_.activeProfileId);
    project_->setDefaultProfileId(profiles_.activeProfileId);
    refreshProfileBox();
    return profiles_;
}

void MainWindow::exportMixDialog()
{
    const auto& model = project_->model();
    if (model.tracks.empty()) {
        QMessageBox::information(this, tr("Export Mix"), tr("Import some stems first."));
        return;
    }
    if (project_->isBusy()) {
        QMessageBox::information(this, tr("Export Mix"), tr("Stems are still loading. Try again in a moment."));
        return;
    }

    QSettings settings;
    ExportDialog::Input input;
    input.library = profiles_;
    input.profileId = model.profileId;
    input.song = model.release;
    input.projectName = project_->displayName();
    input.formatIndex = settings.value(QStringLiteral("export/format"), 0).toInt();
    input.suggestedDirectory = settings.value(QStringLiteral("export/lastDir")).toString();
    if (input.suggestedDirectory.isEmpty() && !project_->isUntitled())
        input.suggestedDirectory = QString::fromStdString(daw::pathToUtf8(project_->root() / "Exports"));

    int muted = 0, soloed = 0;
    for (const auto& t : model.tracks) {
        muted += t.muted ? 1 : 0;
        soloed += t.soloed ? 1 : 0;
    }
    if (soloed > 0)
        input.mixNote = tr("Solo is active: only the %n soloed track(s) will be in the export.", nullptr, soloed);
    else if (muted > 0)
        input.mixNote = tr("%n muted track(s) will be left out of the export.", nullptr, muted);

    ExportDialog dialog(std::move(input), [this](const std::string& id) { return editProfiles(id); }, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    project_->setProfileId(dialog.profileId());
    project_->setReleaseMetadata(dialog.song());
    refreshProfileBox();
    const QString path = dialog.path();
    QDir().mkpath(QFileInfo(path).absolutePath());
    settings.setValue(QStringLiteral("export/format"), dialog.formatIndex());
    settings.setValue(QStringLiteral("export/lastDir"), QFileInfo(path).absolutePath());

    daw::ExportRequest request;
    request.path = daw::utf8ToPath(path.toStdString());
    request.format = dialog.format();
    request.dither = true;
    request.metadata = dialog.resolved();
    request.software = "Stem DAWg " DAW_APP_VERSION;
    busyLabel_->setText(tr("Rendering..."));
    project_->exportMix(std::move(request));
}

void MainWindow::openPaths(const QStringList& paths)
{
    QStringList audioFiles;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QString error;
            const bool recover = ProjectController::hasRecoverableAutosave(info.absoluteFilePath())
                                 && QMessageBox::question(this, tr("Recover autosave"),
                                                          tr("This project has autosaved changes newer than the last save. Recover them?"))
                                        == QMessageBox::Yes;
            if (!project_->openProject(info.absoluteFilePath(), recover, &error))
                QMessageBox::warning(this, tr("Open failed"), error);
        } else if (info.isFile()) {
            audioFiles << info.absoluteFilePath();
        } else {
            statusBar()->showMessage(tr("Not found: %1").arg(path), 6000);
        }
    }
    if (!audioFiles.isEmpty())
        project_->importAudio(audioFiles);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!confirmDiscardChanges()) {
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    QStringList files;
    for (const QUrl& url : event->mimeData()->urls())
        if (url.isLocalFile())
            files << url.toLocalFile();
    if (!files.isEmpty())
        project_->importAudio(files);
}
