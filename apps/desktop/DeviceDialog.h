// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/engine/AudioDeviceService.h>

#include <QDialog>

class QComboBox;
class QLabel;

/// Audio device selection: driver type, output device, sample rate, buffer size.
class DeviceDialog final : public QDialog {
    Q_OBJECT

public:
    DeviceDialog(daw::AudioDeviceService& devices, const daw::AudioDeviceSetup& current, QWidget* parent = nullptr);

    daw::AudioDeviceSetup selectedSetup() const;

private:
    void populateDevices();
    void populateDetails();

    daw::AudioDeviceService& devices_;
    daw::AudioDeviceSetup initial_;
    QComboBox* type_ = nullptr;
    QComboBox* device_ = nullptr;
    QComboBox* rate_ = nullptr;
    QComboBox* buffer_ = nullptr;
    QLabel* info_ = nullptr;
};
