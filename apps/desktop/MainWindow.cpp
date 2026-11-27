// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "MainWindow.h"

#include "AboutDialog.h"
#include "CleanupPanel.h"
#include "MasterPanel.h"
#include "DeviceDialog.h"
#include "DiagnosticsPanel.h"
#include "EngineBridge.h"
#include "ExportDialog.h"
#include "HelpText.h"
#include "LoudnessDisplay.h"
#include "ProfileDialog.h"
#include "SoundFontDialog.h"
#include "ProjectController.h"
#include "StemReportPanel.h"
#include "TrackPanel.h"
#include "TrackStyle.h"
#include "VoiceModelDialog.h"
#include "WaveformView.h"

#include <daw/ai/AnalysisService.h>
#include <daw/ai/Analyzers.h>
#include <daw/ai/BuiltinModels.h>
#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/Midi.h>
#include <daw/session/SnapshotBuilder.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
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
    patterns << QStringLiteral("*.mid") << QStringLiteral("*.midi");
    return QObject::tr("Audio and MIDI files (%1)").arg(patterns.join(' '));
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
    {
        QString soundFont = QSettings().value(QStringLiteral("midi/soundFont")).toString();
        if (soundFont.isEmpty() && QFileInfo::exists(SoundFontDialog::defaultInstallPath()))
            soundFont = SoundFontDialog::defaultInstallPath();
        project_->setSoundFont(soundFont);
    }
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
    analyze->setToolTip(helpTip(tr("Analyze"),
                                tr("Builds the stem report: loudness, noise beds, shared low end and bleed between stems, with advice.\n"
                                   "Also measures each file in detail (Analysis Log tab) and marks drum hits in yellow.\n"
                                   "Runs on its own after importing. Analysis never changes the audio.")));

    bypassCleanup_ = new QAction(tr("Bypass Clean-up"), this);
    bypassCleanup_->setCheckable(true);
    bypassCleanup_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    bypassCleanup_->setToolTip(helpTip(tr("Bypass all clean-up (Ctrl+B)"),
                                       tr("Plays every track without its clean-up chain, so you can compare with the original.\n"
                                          "For listening only: exports and the stem report always include the clean-up.")));
    connect(bypassCleanup_, &QAction::toggled, this, [this](bool on) { project_->setCleanupBypassed(on); });
    transport->addAction(bypassCleanup_);

    listenRemoved_ = new QAction(tr("Listen to Removed"), this);
    listenRemoved_->setCheckable(true);
    listenRemoved_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    listenRemoved_->setToolTip(helpTip(tr("Listen to removed (Ctrl+Shift+B)"),
                                       tr("Plays only what the clean-up takes away: the noise, hum and bed that are removed.\n"
                                          "If you hear music in it, the clean-up is too strong. Tracks without clean-up are silent.\n"
                                          "For listening only: exports are not affected.")));
    connect(listenRemoved_, &QAction::toggled, this, [this](bool on) { project_->setListenRemoved(on); });
    if (auto* button = qobject_cast<QToolButton*>(transport->widgetForAction(bypassCleanup_)))
        button->setStyleSheet(QStringLiteral("QToolButton:checked { background: #e0a030; color: #161616; border-radius: 3px; }"));

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
    transport->addSeparator();
    loudness_ = new LoudnessDisplay(this);
    transport->addWidget(loudness_);
    connect(loudness_, &LoudnessDisplay::resetRequested, this, [this] { engine_->engine().resetLoudness(); });
    connect(clipLight_, &QToolButton::clicked, this, [this] { setClipIndicator(false); });
    connect(masterFader_, &VolumeFader::gainDbChanged, this, [this](double db) {
        masterLabel_->setText(VolumeFader::formatDb(db));
        project_->setMasterGain(db);
    });
    connect(analyze, &QAction::triggered, this, &MainWindow::analyzeEverything);

    cleanupPanel_ = new CleanupPanel(this);
    cleanupPanel_->setListenAction(listenRemoved_);
    cleanupDock_ = new QDockWidget(tr("Clean-up"), this);
    cleanupDock_->setObjectName(QStringLiteral("cleanup"));
    cleanupDock_->setWidget(cleanupPanel_);
    addDockWidget(Qt::RightDockWidgetArea, cleanupDock_);

    diagnostics_ = new DiagnosticsPanel(this);
    auto* diagDock = new QDockWidget(tr("Diagnostics"), this);
    diagDock->setObjectName(QStringLiteral("diagnostics"));
    diagDock->setWidget(diagnostics_);
    addDockWidget(Qt::RightDockWidgetArea, diagDock);
    tabifyDockWidget(cleanupDock_, diagDock);

    masterPanel_ = new MasterPanel(this);
    masterDock_ = new QDockWidget(tr("Master"), this);
    masterDock_->setObjectName(QStringLiteral("master"));
    masterDock_->setWidget(masterPanel_);
    addDockWidget(Qt::RightDockWidgetArea, masterDock_);
    tabifyDockWidget(cleanupDock_, masterDock_);
    cleanupDock_->raise();

    stemPanel_ = new StemReportPanel(this);
    auto* stemDock = new QDockWidget(tr("Stem Report"), this);
    stemDock->setObjectName(QStringLiteral("stemReport"));
    stemDock->setWidget(stemPanel_);
    addDockWidget(Qt::BottomDockWidgetArea, stemDock);

    analysisView_ = new QPlainTextEdit(this);
    analysisView_->setReadOnly(true);
    analysisView_->setFont(QFont(QStringLiteral("Consolas"), 9));
    auto* analysisDock = new QDockWidget(tr("Analysis Log"), this);
    analysisDock->setObjectName(QStringLiteral("analysis"));
    analysisDock->setWidget(analysisView_);
    addDockWidget(Qt::BottomDockWidgetArea, analysisDock);
    tabifyDockWidget(stemDock, analysisDock);
    stemDock->raise();
    resizeDocks({stemDock}, {240}, Qt::Vertical);
    resizeDocks({cleanupDock_}, {380}, Qt::Horizontal); // room for the clean-up buttons

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
    file->addAction(tr("&Import Audio or MIDI..."), QKeySequence(Qt::CTRL | Qt::Key_I), this, &MainWindow::importAudio)
        ->setToolTip(tr("Add WAV, FLAC, AIFF, OGG or MP3 stems, or MIDI files, as new tracks. They are copied into the project.\n"
                        "MIDI files play through a SoundFont (Settings > SoundFont for MIDI)."));
    file->addAction(tr("&Export Mix..."), QKeySequence(Qt::CTRL | Qt::Key_E), this, &MainWindow::exportMixDialog)
        ->setToolTip(tr("Render the whole song to a WAV file with your artist profile's metadata."));
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    file->setToolTipsVisible(true);

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = editMenu->addAction(tr("&Undo"), QKeySequence::Undo, this, [this] { project_->undo(); });
    redoAction_ = editMenu->addAction(tr("&Redo"), this, [this] { project_->redo(); });
    redoAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    refreshUndoActions();

    auto* audio = menuBar()->addMenu(tr("&Audio"));
    audio->addAction(tr("&Device Settings..."), this, &MainWindow::showDeviceDialog)
        ->setToolTip(tr("Choose the driver, output device, sample rate and buffer size."));
    audio->setToolTipsVisible(true);

    auto* settings = menuBar()->addMenu(tr("&Settings"));
    settings->addAction(tr("Artist &Profiles..."), this, [this] { editProfiles(project_->model().profileId); })
        ->setToolTip(tr("Enter your artist details once; they are written into every export."));
    settings->addAction(tr("&SoundFont for MIDI..."), this, [this] { showSoundFontDialog(); })
        ->setToolTip(tr("Choose or download the instrument sounds MIDI tracks play with (FluidR3_GM, or any .sf2 file)."));
    settings->setToolTipsVisible(true);

    auto* analysis = menuBar()->addMenu(tr("A&nalysis"));
    analysis->addAction(tr("&Stem Report"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] { project_->runStemReport(); })
        ->setToolTip(tr("Measure every stem and the mix (loudness, noise beds, shared low end, bleed) and give advice."));
    analysis->addAction(tr("&Detailed File Analysis"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), this,
                        [this] { project_->analyzeAll(); })
        ->setToolTip(tr("Measure levels, hits and frequency balance of every file (Analysis Log tab). Does not change the audio."));
    // Drums to MIDI acts on the first track named like a drum stem, or else
    // the track the Clean-up panel shows. (Right-click a track name for any
    // other track.)
    auto* drumsMenu = analysis->addMenu(tr("&Drums to MIDI"));
    drumsMenu->setToolTipsVisible(true);
    const auto drumsToMidi = [this](int grid) {
        const auto& model = project_->model();
        QString id;
        for (const auto& t : model.tracks)
            if (QString::fromStdString(t.name).contains(QStringLiteral("drum"), Qt::CaseInsensitive)) {
                id = QString::fromStdString(t.id.str());
                break;
            }
        if (id.isEmpty())
            id = cleanupPanel_->currentTrackId();
        if (id.isEmpty()) {
            statusBar()->showMessage(tr("No drum track found: right-click the drum track's name and choose Drums to MIDI."), 6000);
            return;
        }
        project_->drumsToMidi(daw::TrackId(id.toStdString()), grid);
    };
    const QString drumsTip = tr("Transcribe the drum track (kick, snare, hi-hats, crash) to a General MIDI file on the song's beats, "
                                "and add it as a MIDI track. For another track, right-click its name.");
    drumsMenu->addAction(tr("Quantise to 1/&16 Notes"), this, [drumsToMidi] { drumsToMidi(16); })->setToolTip(drumsTip);
    drumsMenu->addAction(tr("Quantise to 1/&32 Notes"), this, [drumsToMidi] { drumsToMidi(32); })->setToolTip(drumsTip);
    drumsMenu->addAction(tr("&As Played"), this, [drumsToMidi] { drumsToMidi(0); })->setToolTip(drumsTip);
    // Notes to MIDI acts on the track the Clean-up panel shows.
    auto* notesMenu = analysis->addMenu(tr("&Notes to MIDI"));
    notesMenu->setToolTipsVisible(true);
    const auto notesToMidi = [this](int grid) {
        const QString id = cleanupPanel_->currentTrackId();
        if (id.isEmpty()) {
            statusBar()->showMessage(tr("Choose a track: right-click its name and choose Notes to MIDI."), 6000);
            return;
        }
        project_->notesToMidi(daw::TrackId(id.toStdString()), grid);
    };
    const QString notesTip = tr("Transcribe the notes of the track the Clean-up panel shows (bass, guitar, keys, synth, voice) to a "
                                "MIDI file, on the song's beats when there is a drum track. For any track, right-click its name.");
    notesMenu->addAction(tr("&As Played"), this, [notesToMidi] { notesToMidi(0); })->setToolTip(notesTip);
    notesMenu->addAction(tr("Quantise to 1/&16 Notes"), this, [notesToMidi] { notesToMidi(16); })->setToolTip(notesTip);
    analysis->setToolTipsVisible(true);

    auto* cleanup = menuBar()->addMenu(tr("&Clean-up"));
    cleanup->addAction(tr("Clean-up &Panel"), QKeySequence(Qt::CTRL | Qt::Key_L), this, [this] { showCleanupPanel(); })
        ->setStatusTip(tr("Edit the selected track's high-pass, expander and DC removal"));
    cleanup->addAction(tr("Apply &All Stem Report Fixes"), this, [this] {
        if (const daw::StemReport* report = project_->stemReport())
            project_->applyCleanupFixes(daw::allCleanupFixes(*report));
        else
            statusBar()->showMessage(tr("Run the stem report first (Ctrl+R)"), 6000);
    })->setStatusTip(tr("Set up every clean-up the stem report suggests"));
    cleanup->addAction(tr("Suppress &Bleed in All Stems"), this, [this] {
        const auto tracks = daw::bleedTracks(project_->model());
        if (tracks.size() < 2) {
            statusBar()->showMessage(tr("Bleed suppression compares stems: import at least two tracks of recorded audio."), 6000);
            return;
        }
        project_->suppressBleed(tracks, daw::kBleedStrengths[1]);
    })->setStatusTip(tr("Turn each stem down where another stem owns the sound (medium strength; renders copies, Ctrl+Z undoes)"));
    cleanup->addSeparator();
    cleanup->addAction(bypassCleanup_);
    cleanup->addAction(listenRemoved_);

    auto* mastering = menuBar()->addMenu(tr("&Master"));
    mastering->addAction(tr("Master &Panel"), QKeySequence(Qt::CTRL | Qt::Key_M), this, [this] {
        masterDock_->show();
        masterDock_->raise();
    })->setStatusTip(tr("The master bus: true-peak limiter and loudness target"));
    mastering->addAction(tr("&Match Loudness"), this, [this] { project_->matchLoudness(project_->model().master.targetLufs); })
        ->setStatusTip(tr("Set the master fader for the loudness target through the limiter"));

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
    connect(project_.get(), &ProjectController::stemReportChanged, this, &MainWindow::refreshStemReport);
    connect(project_.get(), &ProjectController::stemReportProgress, stemPanel_, &StemReportPanel::setProgress);
    connect(project_.get(), &ProjectController::stemReportFailed, this,
            [this](const QString& message) { statusBar()->showMessage(tr("Stem report failed: %1").arg(message), 8000); });
    connect(stemPanel_, &StemReportPanel::analyzeRequested, this, [this] { project_->runStemReport(); });
    connect(stemPanel_, &StemReportPanel::applyFixesRequested, this,
            [this](const std::vector<daw::CleanupFix>& fixes) { project_->applyCleanupFixes(fixes); });
    connect(stemPanel_, &StemReportPanel::trackActivated, this, [this](int index) {
        const daw::StemReport* report = project_->stemReport();
        if (report != nullptr && index >= 0 && static_cast<std::size_t>(index) < report->tracks.size())
            cleanupPanel_->selectTrack(QString::fromStdString(report->tracks[static_cast<std::size_t>(index)].track.str()));
    });
    connect(masterPanel_, &MasterPanel::settingsEdited, this,
            [this](const daw::MasterSettings& settings) { project_->setMasterSettings(settings); });
    connect(masterPanel_, &MasterPanel::matchRequested, this, [this](double target) {
        masterPanel_->setMatching(true);
        project_->matchLoudness(target);
    });
    connect(project_.get(), &ProjectController::loudnessMatched, this, [this](const daw::LoudnessMatch& match, double target) {
        masterPanel_->setMatching(false);
        masterPanel_->showMatch(match, target);
    });
    connect(project_.get(), &ProjectController::projectChanged, this, [this] {
        masterPanel_->setMatching(false);
        masterPanel_->setTonalBusy(false);
    });
    connect(masterPanel_, &MasterPanel::newTargetRequested, this, [this](const QString& folder) { project_->buildTonalTarget(folder); });
    connect(masterPanel_, &MasterPanel::compareRequested, this, [this] { project_->compareTonalBalance(); });
    connect(project_.get(), &ProjectController::tonalLibraryChanged, this,
            [this] { masterPanel_->setTargetLibrary(project_->tonalTargetLibrary()); });
    connect(project_.get(), &ProjectController::tonalBusy, masterPanel_, &MasterPanel::setTonalBusy);
    connect(project_.get(), &ProjectController::tonalCompared, masterPanel_, &MasterPanel::showComparison);
    connect(project_.get(), &ProjectController::tonalFailed, masterPanel_, &MasterPanel::showTonalError);
    masterPanel_->setTargetLibrary(project_->tonalTargetLibrary());
    connect(project_.get(), &ProjectController::loudnessMatchFailed, this, [this](const QString& message) {
        masterPanel_->setMatching(false);
        masterPanel_->showMatchError(message);
    });
    connect(cleanupPanel_, &CleanupPanel::cleanupEdited, this, [this](const QString& id, const daw::TrackCleanup& cleanup) {
        project_->setTrackCleanup(daw::TrackId(id.toStdString()), cleanup);
    });
    connect(cleanupPanel_, &CleanupPanel::learnNoiseRequested, this,
            [this](const QString& id) { project_->learnNoiseProfile(daw::TrackId(id.toStdString()), timeSelection()); });
    connect(cleanupPanel_, &CleanupPanel::detectHumRequested, this,
            [this](const QString& id) { project_->detectHum(daw::TrackId(id.toStdString()), timeSelection()); });
    connect(cleanupPanel_, &CleanupPanel::voiceDenoiseRequested, this, [this](const QString& id, double reductionDb) {
        if (!daw::isVoiceModelInstalled()) {
            VoiceModelDialog dialog(this);
            if (dialog.exec() != QDialog::Accepted)
                return;
        }
        project_->voiceDenoise(daw::TrackId(id.toStdString()), reductionDb);
    });
    connect(cleanupPanel_, &CleanupPanel::revertRequested, this,
            [this](const QString& id) { project_->revertProcessing(daw::TrackId(id.toStdString())); });
    connect(cleanupPanel_, &CleanupPanel::bleedRequested, this, [this](const QString& id, double reductionDb) {
        const std::vector<daw::TrackId> tracks =
            id.isEmpty() ? daw::bleedTracks(project_->model()) : std::vector<daw::TrackId>{daw::TrackId(id.toStdString())};
        project_->suppressBleed(tracks, daw::bleedStrengthFor(reductionDb));
    });
    connect(project_.get(), &ProjectController::bleedLossWarning, this, [this](const QStringList& names, double worstDb) {
        QStringList escaped;
        for (const QString& name : names)
            escaped << name.toHtmlEscaped();
        QMessageBox::information(this, tr("Bleed suppression"),
                                 tr("<p>Bleed suppression turned the louder parts of <b>%1</b> down by up to %2 dB.</p>"
                                    "<p>Most of what these stems hold is also in other stems: often a duplicated kick, a "
                                    "doubled part or separation residue. Listen: if something is missing, click "
                                    "<b>Original</b> (or Ctrl+Z) or try a lighter strength.</p>")
                                     .arg(escaped.join(QStringLiteral(", ")))
                                     .arg(-worstDb, 0, 'f', 1));
    });
    connect(project_.get(), &ProjectController::voiceLossWarning, this, [this](const QString& name, double voiceChangeDb) {
        QMessageBox::warning(this, tr("Voice denoise"),
                             tr("<p>The voice model also turned the loudest parts of <b>%1</b> down by %2 dB.</p>"
                                "<p>It is made for speech and can take a sung or heavily effected voice for noise. "
                                "Listen to the track: if the voice suffers, click <b>Original</b> (or Ctrl+Z) and use "
                                "<b>Noise reduction</b> instead, or try a gentler reduction.</p>")
                                 .arg(name.toHtmlEscaped())
                                 .arg(-voiceChangeDb, 0, 'f', 1));
    });
    connect(waveform_, &WaveformView::selectionChanged, this, [this](qint64 start, qint64 end) {
        cleanupPanel_->setSelectionActive(end > start);
        const double rate = project_->model().sampleRate;
        if (end > start && rate > 0.0)
            statusBar()->showMessage(tr("Selected %1 s from %2. Learn Noise and Detect now measure the selection; Esc clears it.")
                                         .arg(static_cast<double>(end - start) / rate, 0, 'f', 2)
                                         .arg(formatPosition(start, rate, project_->model().tempo.bpm, project_->model().tempo.numerator)),
                                     6000);
    });
    connect(waveform_, &WaveformView::laneClicked, this, [this](int lane) {
        const auto& tracks = project_->model().tracks;
        if (lane >= 0 && static_cast<std::size_t>(lane) < tracks.size())
            cleanupPanel_->selectTrack(QString::fromStdString(tracks[static_cast<std::size_t>(lane)].id.str()));
    });
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
    connect(trackPanel_, &TrackPanel::soloToggled, this, [this](const QString& id, bool on, bool additive) {
        project_->setTrackSoloed(daw::TrackId(id.toStdString()), on, !additive);
    });
    connect(trackPanel_, &TrackPanel::cleanupRequested, this, [this](const QString& id) { showCleanupPanel(id); });
    connect(trackPanel_, &TrackPanel::renameRequested, this, &MainWindow::renameTrackDialog);
    connect(trackPanel_, &TrackPanel::drumsToMidiRequested, this,
            [this](const QString& id, int grid) { project_->drumsToMidi(daw::TrackId(id.toStdString()), grid); });
    connect(trackPanel_, &TrackPanel::notesToMidiRequested, this,
            [this](const QString& id, int grid) { project_->notesToMidi(daw::TrackId(id.toStdString()), grid); });
    const auto midiResult = [this](const QString& title) {
        return [this, title](bool ok, const QString& message, const QString& path) {
            if (!ok) {
                QMessageBox::warning(this, title, message);
                return;
            }
            statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)), 8000);
            QMessageBox box(QMessageBox::Information, title, message, QMessageBox::Ok, this);
            auto* show = box.addButton(tr("Show in Folder"), QMessageBox::ActionRole);
            box.exec();
            if (box.clickedButton() == show)
                QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
        };
    };
    connect(project_.get(), &ProjectController::drumsToMidiFinished, this, midiResult(tr("Drums to MIDI")));
    connect(project_.get(), &ProjectController::notesToMidiFinished, this, midiResult(tr("Notes to MIDI")));
    connect(trackPanel_, &TrackPanel::deleteRequested, this,
            [this](const QString& id) { project_->removeTrack(daw::TrackId(id.toStdString())); });
    connect(project_.get(), &ProjectController::undoChanged, this, &MainWindow::refreshUndoActions);
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

    loudness_->setReading(engine_->engine().loudness(), transport.playing);

    const auto peaks = engine_->engine().takeMeterPeaks();
    if (peaks.left >= 1.0f || peaks.right >= 1.0f)
        setClipIndicator(true);
    if (diagnostics_->isVisible())
        diagnostics_->updateEngine(engine_->engine().diagnostics(), engine_->devices().status(), peaks);
    if (masterPanel_->isVisible())
        masterPanel_->setLimiterReduction(engine_->engine().takeMasterReductionDb());
    if (cleanupPanel_->isVisible()) {
        const QString id = cleanupPanel_->currentTrackId();
        cleanupPanel_->setGainReduction(
            id.isEmpty() ? 0.0f : engine_->engine().takeCleanupReductionDb(daw::trackPlaybackKey(daw::TrackId(id.toStdString()))));
    }
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
        state.cleanup = !track.cleanup.hasStages()        ? TrackHeaderState::Cleanup::Off
                        : track.cleanup.active() && !project_->isCleanupBypassed() ? TrackHeaderState::Cleanup::On
                                                                                   : TrackHeaderState::Cleanup::Bypassed;
        state.cleanupSummary = QString::fromStdString(daw::describeCleanup(track.cleanup));
        audible.push_back(state.audible);
        headers.push_back(std::move(state));
    }
    trackPanel_->setTracks(headers);
    waveform_->setLaneAudibility(audible);
    if (masterFader_->gainDb() != model.masterGainDb && !masterFader_->isSliderDown())
        masterFader_->setGainDb(model.masterGainDb);
    masterLabel_->setText(VolumeFader::formatDb(model.masterGainDb));
    masterPanel_->setSettings(model.master, model.masterGainDb);
    if (bypassCleanup_->isChecked() != project_->isCleanupBypassed()) {
        const QSignalBlocker block(bypassCleanup_);
        bypassCleanup_->setChecked(project_->isCleanupBypassed());
    }
    if (listenRemoved_->isChecked() != project_->isListeningRemoved()) {
        const QSignalBlocker block(listenRemoved_);
        listenRemoved_->setChecked(project_->isListeningRemoved());
    }
    refreshCleanupPanel();
}

void MainWindow::refreshCleanupPanel()
{
    const auto& model = project_->model();
    const daw::StemReport* report = project_->stemReport();
    std::vector<CleanupTrackInfo> tracks;
    tracks.reserve(model.tracks.size());
    const std::vector<daw::TrackId> bleedTracks = daw::bleedTracks(model);
    for (std::size_t i = 0; i < model.tracks.size(); ++i) {
        const auto& track = model.tracks[i];
        CleanupTrackInfo info;
        info.id = QString::fromStdString(track.id.str());
        info.name = QString::fromStdString(track.name);
        info.color = trackColor(info.name, static_cast<int>(i));
        info.cleanup = track.cleanup;
        if (const auto voice = daw::trackVoiceDenoise(project_->model(), track.id))
            info.voiceDenoiseDb = voice->amountDb;
        if (const auto bleed = daw::trackBleedSuppression(project_->model(), track.id))
            info.bleedDb = bleed->amountDb;
        info.bleedAvailable = bleedTracks.size() >= 2
                              && std::find(bleedTracks.begin(), bleedTracks.end(), track.id) != bleedTracks.end();
        info.renderRunning = project_->isRenderRunning();
        if (report != nullptr)
            for (const auto& measured : report->tracks)
                if (measured.track == track.id) {
                    info.noiseFloorDbfs = measured.noiseFloorDbfs;
                    info.floorAfterExpander = measured.cleanup.active() && measured.cleanup.expander.enabled;
                }
        tracks.push_back(std::move(info));
    }
    cleanupPanel_->setTracks(tracks);
}

void MainWindow::refreshUndoActions()
{
    undoAction_->setEnabled(project_->canUndo());
    undoAction_->setText(project_->canUndo() ? tr("&Undo %1").arg(project_->undoText()) : tr("&Undo"));
    redoAction_->setEnabled(project_->canRedo());
    redoAction_->setText(project_->canRedo() ? tr("&Redo %1").arg(project_->redoText()) : tr("&Redo"));
}

void MainWindow::renameTrackDialog(const QString& trackId)
{
    const daw::Track* track = project_->model().findTrack(daw::TrackId(trackId.toStdString()));
    if (track == nullptr)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Track"), tr("Track name:"), QLineEdit::Normal,
                                               QString::fromStdString(track->name), &ok);
    if (ok)
        project_->renameTrack(daw::TrackId(trackId.toStdString()), name);
}

std::optional<daw::FrameRange> MainWindow::timeSelection() const
{
    if (!waveform_->hasSelection())
        return std::nullopt;
    return daw::FrameRange{waveform_->selectionStart(), waveform_->selectionEnd()};
}

void MainWindow::showCleanupPanel(const QString& trackId)
{
    if (!trackId.isEmpty())
        cleanupPanel_->selectTrack(trackId);
    cleanupDock_->show();
    cleanupDock_->raise();
}

void MainWindow::refreshStemReport()
{
    const daw::StemReport* report = project_->stemReport();
    std::vector<QColor> colors;
    if (report != nullptr)
        for (std::size_t i = 0; i < report->tracks.size(); ++i)
            colors.push_back(trackColor(QString::fromStdString(report->tracks[i].name), static_cast<int>(i)));
    stemPanel_->setReport(report, colors, project_->isStemReportStale(), project_->isStemReportRunning());
    refreshCleanupPanel();
}

void MainWindow::analyzeEverything()
{
    project_->runStemReport();
    project_->analyzeAll();
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
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Audio or MIDI"),
                                                            settings.value(QStringLiteral("import/lastDir")).toString(),
                                                            audioFileFilter());
    if (files.isEmpty())
        return;
    settings.setValue(QStringLiteral("import/lastDir"), QFileInfo(files.front()).absolutePath());
    importFiles(files);
}

void MainWindow::importFiles(const QStringList& files)
{
    QStringList accepted;
    int skippedMidi = 0;
    bool askedForFont = false, haveFont = false;
    for (const QString& file : files) {
        if (daw::isMidiFile(daw::utf8ToPath(file.toStdString()))) {
            if (!askedForFont) {
                askedForFont = true;
                haveFont = ensureSoundFont(tr("MIDI files need a SoundFont to make sound."));
            }
            if (!haveFont) {
                ++skippedMidi;
                continue;
            }
        }
        accepted << file;
    }
    if (skippedMidi > 0)
        statusBar()->showMessage(tr("%n MIDI file(s) skipped: no SoundFont is set up.", nullptr, skippedMidi), 8000);
    if (!accepted.isEmpty())
        project_->importAudio(accepted);
}

bool MainWindow::ensureSoundFont(const QString& reason)
{
    if (!project_->soundFont().isEmpty() && QFileInfo::exists(project_->soundFont()))
        return true;
    showSoundFontDialog(reason);
    return !project_->soundFont().isEmpty() && QFileInfo::exists(project_->soundFont());
}

void MainWindow::showSoundFontDialog(const QString& reason)
{
    SoundFontDialog dialog(project_->soundFont(), reason, this);
    dialog.exec();
    const QString chosen = dialog.soundFontPath();
    if (chosen == project_->soundFont())
        return;
    QSettings().setValue(QStringLiteral("midi/soundFont"), chosen);
    project_->setSoundFont(chosen);
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
    input.destinationIndex = settings.value(QStringLiteral("export/destination"), 0).toInt();
    input.projectRate = model.sampleRate;
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
    settings.setValue(QStringLiteral("export/destination"), dialog.destinationIndex());
    settings.setValue(QStringLiteral("export/lastDir"), QFileInfo(path).absolutePath());

    daw::ExportRequest request;
    request.path = daw::utf8ToPath(path.toStdString());
    request.format = dialog.format();
    request.dither = true;
    request.metadata = dialog.resolved();
    request.software = "Stem DAWg " DAW_APP_VERSION;
    busyLabel_->setText(tr("Rendering..."));
    const auto destinations = dialog.destinations();
    if (destinations.empty()) {
        project_->exportMix(std::move(request));
        return;
    }
    // One destination: the path as typed (the suggestion carries its name).
    // Several: each file is named after its destination.
    std::vector<ProjectController::DestinationFile> files;
    for (const daw::Destination* d : destinations)
        files.push_back({d, destinations.size() == 1 ? request.path : daw::destinationPath(request.path, *d)});
    project_->exportForDestinations(std::move(request), std::move(files));
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
        importFiles(audioFiles);
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
        importFiles(files);
}
