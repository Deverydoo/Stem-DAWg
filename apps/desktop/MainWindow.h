// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/project/Metadata.h>

#include <QIcon>
#include <QMainWindow>

#include <memory>

namespace daw {
class AnalysisService;
}

class DiagnosticsPanel;
class EngineBridge;
class ProjectController;
class QAction;
class QComboBox;
class QToolButton;
class VolumeFader;
class QLabel;
class QPlainTextEdit;
class QScrollBar;
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
    void togglePlayPause();
    void stopAndReturnToStart();
    void updateTitle();

    bool confirmDiscardChanges();
    void newProject();
    void openProject();
    bool saveProject();
    bool saveProjectAs();
    void importAudio();
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
