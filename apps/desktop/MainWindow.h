// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/project/Metadata.h>
#include <daw/session/NoiseTools.h>

#include <QIcon>
#include <QMainWindow>

#include <memory>
#include <optional>

namespace daw {
class AnalysisService;
}

class CleanupPanel;
class DiagnosticsPanel;
class EngineBridge;
class LoudnessDisplay;
class ProjectController;
class QAction;
class QComboBox;
class QDockWidget;
class QToolButton;
class VolumeFader;
class QLabel;
class QPlainTextEdit;
class QScrollBar;
class StemReportPanel;
class TrackPanel;
class WaveformView;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    /// Handles command-line / "Open with" arguments: a project folder is
    /// opened, audio files are imported into the current project.
    void openPaths(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildUi();
    void buildMenus();
    void connectSignals();
    void startAi();
    void startAudio();

    void onTick();
    void refreshLanes();
    void refreshMixer();
    void refreshStemReport();
    void refreshCleanupPanel();
    void refreshUndoActions();
    void renameTrackDialog(const QString& trackId);
    void showCleanupPanel(const QString& trackId = {});
    /// The waveform's time selection, if any.
    std::optional<daw::FrameRange> timeSelection() const;
    void analyzeEverything();
    void togglePlayPause();
    void stopAndReturnToStart();
    void updateTitle();

    bool confirmDiscardChanges();
    void newProject();
    void openProject();
    bool saveProject();
    bool saveProjectAs();
    void importAudio();
    /// Imports audio and MIDI files; asks for a SoundFont first when MIDI is included.
    void importFiles(const QStringList& files);
    bool ensureSoundFont(const QString& reason);
    void showSoundFontDialog(const QString& reason = {});
    void showDeviceDialog();
    void exportMixDialog();
    std::optional<daw::ProfileLibrary> editProfiles(const std::string& selectId);
    void saveProfiles();
    void refreshProfileBox();
    void setClipIndicator(bool clipped);

    std::unique_ptr<EngineBridge> engine_;
    std::unique_ptr<daw::AnalysisService> analysis_;
    std::unique_ptr<ProjectController> project_;

    WaveformView* waveform_ = nullptr;
    TrackPanel* trackPanel_ = nullptr;
    QScrollBar* laneScroll_ = nullptr;
    DiagnosticsPanel* diagnostics_ = nullptr;
    QPlainTextEdit* analysisView_ = nullptr;
    StemReportPanel* stemPanel_ = nullptr;
    CleanupPanel* cleanupPanel_ = nullptr;
    QDockWidget* cleanupDock_ = nullptr;
    QAction* bypassCleanup_ = nullptr;
    QAction* listenRemoved_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    LoudnessDisplay* loudness_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* busyLabel_ = nullptr;
    QAction* playAction_ = nullptr;
    QComboBox* profileBox_ = nullptr;
    VolumeFader* masterFader_ = nullptr;
    QLabel* masterLabel_ = nullptr;
    QToolButton* clipLight_ = nullptr;
    bool clipped_ = false;
    daw::ProfileLibrary profiles_;
    QIcon playIcon_;
    QIcon pauseIcon_;
    bool shownPlaying_ = false;
    int logSink_ = 0;
    QString aiStatus_;
};
