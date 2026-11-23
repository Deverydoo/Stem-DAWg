# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

# Windows resources (application icon + version info).
set(DAW_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/stem-dawg.ico")
file(TO_CMAKE_PATH "${DAW_ICON_PATH}" DAW_ICON_PATH)
configure_file(resources/stem-dawg.rc.in "${CMAKE_CURRENT_BINARY_DIR}/stem-dawg.rc" @ONLY)

qt_add_executable(stem_dawg WIN32
    main.cpp
    MainWindow.h MainWindow.cpp
    EngineBridge.h EngineBridge.cpp
    ProjectController.h ProjectController.cpp
    WaveformView.h WaveformView.cpp
    TrackPanel.h TrackPanel.cpp
    TrackStyle.h
    HelpText.h
    AboutDialog.h AboutDialog.cpp
    ProfileDialog.h ProfileDialog.cpp
    ExportDialog.h ExportDialog.cpp
    DeviceDialog.h DeviceDialog.cpp
    DiagnosticsPanel.h DiagnosticsPanel.cpp
    LoudnessDisplay.h LoudnessDisplay.cpp
    StemReportPanel.h StemReportPanel.cpp
    CleanupPanel.h CleanupPanel.cpp
    SoundFontDialog.h SoundFontDialog.cpp
    VoiceModelDialog.h VoiceModelDialog.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/stem-dawg.rc")

# Window icons (every size, so Qt picks a crisp one per DPI) and the About logo.
qt_add_resources(stem_dawg "branding"
    PREFIX "/branding"
    BASE resources/icons
    FILES
        resources/icons/stem-dawg-16.png
        resources/icons/stem-dawg-20.png
        resources/icons/stem-dawg-24.png
        resources/icons/stem-dawg-32.png
        resources/icons/stem-dawg-40.png
        resources/icons/stem-dawg-48.png
        resources/icons/stem-dawg-64.png
        resources/icons/stem-dawg-96.png
        resources/icons/stem-dawg-128.png
        resources/icons/stem-dawg-256.png
        resources/icons/stem-dawg-logo-about.png)

target_link_libraries(stem_dawg PRIVATE
    Qt6::Widgets
    Qt6::Network
    daw::session
    daw::ai
    daw::compile_options)

# Qt keywords (signals/slots/emit) are disabled to avoid macro clashes with
# third-party headers; use Q_SIGNALS / Q_SLOTS / Q_EMIT.
target_compile_definitions(stem_dawg PRIVATE
    QT_NO_KEYWORDS
    DAW_APP_VERSION="${PROJECT_VERSION}")

set_target_properties(stem_dawg PROPERTIES
    OUTPUT_NAME "StemDAWg"
    FOLDER "apps")

daw_copy_runtime_dlls(stem_dawg)

# Deploy Qt runtime next to the executable so it runs from the build tree.
if(WIN32)
    get_target_property(_daw_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_daw_qt_bin "${_daw_qmake}" DIRECTORY)
    find_program(DAW_WINDEPLOYQT windeployqt HINTS "${_daw_qt_bin}" NO_DEFAULT_PATH)
    if(DAW_WINDEPLOYQT)
        add_custom_command(TARGET stem_dawg POST_BUILD
            COMMAND "${DAW_WINDEPLOYQT}" --no-translations --no-compiler-runtime "$<TARGET_FILE:stem_dawg>"
            COMMENT "Deploying Qt runtime"
            VERBATIM)
    else()
        message(WARNING "windeployqt not found; add the Qt bin directory to PATH to run the app from the build tree.")
    endif()
endif()
