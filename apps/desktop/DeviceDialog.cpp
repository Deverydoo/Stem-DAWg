// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "DeviceDialog.h"

#include "HelpText.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

DeviceDialog::DeviceDialog(daw::AudioDeviceService& devices, const daw::AudioDeviceSetup& current, QWidget* parent)
    : QDialog(parent), devices_(devices), initial_(current)
{
    setWindowTitle(tr("Audio Device"));
    setMinimumWidth(460);

    type_ = new QComboBox(this);
    device_ = new QComboBox(this);
    rate_ = new QComboBox(this);
    buffer_ = new QComboBox(this);
    info_ = new QLabel(this);
    info_->setWordWrap(true);

    auto* form = new QFormLayout;
    const auto row = [&](const QString& label, QWidget* field, const QString& tip) {
        auto* name = new QLabel(label, this);
        name->setToolTip(tip);
        field->setToolTip(tip);
        form->addRow(name, field);
    };
    row(tr("Driver type"), type_,
        helpTip(tr("Driver type"), tr("Windows Audio (WASAPI) works with every device. ASIO gives the lowest latency with "
                                      "pro interfaces. DirectSound is a legacy fallback.")));
    row(tr("Output device"), device_, helpTip(tr("Output device"), tr("Speakers, headphones or audio interface to play through.")));
    row(tr("Sample rate"), rate_,
        helpTip(tr("Sample rate"), tr("Playback rate of the device. Stems at other rates are converted for playback only; "
                                      "exports always use the project's own rate.")));
    row(tr("Buffer size"), buffer_,
        helpTip(tr("Buffer size"), tr("Samples per audio block. Smaller = less delay when you press Play but more CPU risk of "
                                      "dropouts. 256-512 is a good start.")));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(info_);
    layout->addWidget(buttons);

    for (const auto& type : devices_.deviceTypes())
        type_->addItem(QString::fromStdString(type));
    if (const int i = type_->findText(QString::fromStdString(initial_.typeName)); i >= 0)
        type_->setCurrentIndex(i);

    connect(type_, &QComboBox::currentIndexChanged, this, &DeviceDialog::populateDevices);
    connect(device_, &QComboBox::currentIndexChanged, this, &DeviceDialog::populateDetails);
    populateDevices();
}

void DeviceDialog::populateDevices()
{
    const QSignalBlocker block(device_);
    device_->clear();
    for (const auto& name : devices_.outputDevices(type_->currentText().toStdString()))
        device_->addItem(QString::fromStdString(name));
    if (const int i = device_->findText(QString::fromStdString(initial_.outputDeviceName)); i >= 0)
        device_->setCurrentIndex(i);
    populateDetails();
}

void DeviceDialog::populateDetails()
{
    rate_->clear();
    buffer_->clear();
    const auto description = devices_.describe(type_->currentText().toStdString(), device_->currentText().toStdString());
    if (!description) {
        info_->setText(QString::fromStdString(description.error().describe()));
        return;
    }
    for (double rate : description->sampleRates)
        rate_->addItem(QStringLiteral("%1 Hz").arg(rate, 0, 'f', 0), rate);
    for (int size : description->bufferSizes)
        buffer_->addItem(QStringLiteral("%1 samples").arg(size), size);

    if (const int i = rate_->findData(initial_.sampleRate); i >= 0)
        rate_->setCurrentIndex(i);
    const int wantedBuffer = initial_.bufferSize > 0 ? initial_.bufferSize : description->defaultBufferSize;
    if (const int i = buffer_->findData(wantedBuffer); i >= 0)
        buffer_->setCurrentIndex(i);

    info_->setText(tr("%1 output channels. WASAPI shared mode runs at the Windows mixer rate; "
                      "media at other rates is resampled in the background.")
                       .arg(description->numOutputChannels));
}

daw::AudioDeviceSetup DeviceDialog::selectedSetup() const
{
    daw::AudioDeviceSetup setup;
    setup.typeName = type_->currentText().toStdString();
    setup.outputDeviceName = device_->currentText().toStdString();
    setup.sampleRate = rate_->currentData().toDouble();
    setup.bufferSize = buffer_->currentData().toInt();
    return setup;
}
