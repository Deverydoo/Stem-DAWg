// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "MasterPanel.h"

#include "HelpText.h"
#include "ReductionMeter.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>
#include <iterator>

namespace {

constexpr int kCustomPreset = static_cast<int>(std::size(daw::kLoudnessPresets));

QString lufs(double value)
{
    return QStringLiteral("%1 LUFS").arg(value, 0, 'f', 1);
}

} // namespace

MasterPanel::MasterPanel(QWidget* parent) : QWidget(parent)
{
    limiter_ = new QGroupBox(tr("Limiter"), this);
    limiter_->setCheckable(true);
    limiter_->setToolTip(helpTip(tr("True-peak limiter"),
                                 tr("Holds the mix under the ceiling, inter-sample peaks included, so neither a DAC nor\n"
                                    "an MP3 or AAC encoder clips it. The master fader drives the mix into it.")));
    ceiling_ = new QDoubleSpinBox(limiter_);
    ceiling_->setRange(daw::MasterSettings::kMinCeilingDbtp, daw::MasterSettings::kMaxCeilingDbtp);
    ceiling_->setDecimals(1);
    ceiling_->setSingleStep(0.1);
    ceiling_->setSuffix(tr(" dBTP"));
    ceiling_->setKeyboardTracking(false);
    ceiling_->setToolTip(helpTip(tr("Ceiling"), tr("The highest true peak the master may reach. Streaming services ask\n"
                                                   "for -1 dBTP so their encoders do not clip.")));
    release_ = new QDoubleSpinBox(limiter_);
    release_->setRange(daw::MasterSettings::kMinReleaseMs, daw::MasterSettings::kMaxReleaseMs);
    release_->setDecimals(0);
    release_->setSingleStep(10.0);
    release_->setSuffix(tr(" ms"));
    release_->setKeyboardTracking(false);
    release_->setToolTip(helpTip(tr("Release"), tr("How fast the limiter lets go after a peak. Shorter is louder but can\n"
                                                   "distort the low end; longer is smoother. 100 ms suits most songs.")));
    meter_ = new ReductionMeter(limiter_, 12.0);
    meter_->setToolTip(helpTip(tr("Gain reduction"), tr("How far the limiter is turning the mix down right now (during playback).")));
    auto* limiterForm = new QFormLayout(limiter_);
    limiterForm->addRow(tr("Ceiling"), ceiling_);
    limiterForm->addRow(tr("Release"), release_);
    limiterForm->addRow(tr("Reduction"), meter_);

    auto* loudness = new QGroupBox(tr("Loudness"), this);
    preset_ = new QComboBox(loudness);
    for (const daw::LoudnessPreset& p : daw::kLoudnessPresets)
        preset_->addItem(tr(p.name));
    preset_->addItem(tr("Custom"));
    preset_->setToolTip(helpTip(tr("Loudness target"),
                                tr("Where the song is going. Streaming services turn louder songs down to their\n"
                                   "reference, so a louder master only loses dynamics there; club and techno masters\n"
                                   "are usually much louder.")));
    target_ = new QDoubleSpinBox(loudness);
    target_->setRange(daw::MasterSettings::kMinTargetLufs, daw::MasterSettings::kMaxTargetLufs);
    target_->setDecimals(1);
    target_->setSingleStep(0.5);
    target_->setSuffix(tr(" LUFS"));
    target_->setKeyboardTracking(false);
    target_->setToolTip(preset_->toolTip());
    match_ = new QPushButton(tr("Match Loudness"), loudness);
    match_->setToolTip(helpTip(tr("Match loudness"),
                               tr("Measures the mix and sets the master fader so the mastered mix reaches the target\n"
                                  "through the limiter (switched on). Undo restores the fader. The limiter is never\n"
                                  "asked for more than 12 dB: past that it crushes the mix.")));
    fader_ = new QLabel(loudness);
    result_ = new QLabel(loudness);
    result_->setWordWrap(true);
    result_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    auto* loudnessForm = new QFormLayout(loudness);
    loudnessForm->addRow(tr("Target"), preset_);
    loudnessForm->addRow(QString(), target_);
    loudnessForm->addRow(match_);
    loudnessForm->addRow(tr("Master fader"), fader_);
    loudnessForm->addRow(result_);

    auto* hint = new QLabel(tr("The master bus runs after the tracks and their clean-up, on everything you hear and export."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #7d8088;"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(limiter_);
    layout->addWidget(loudness);
    layout->addWidget(hint);
    layout->addStretch(1);

    connect(limiter_, &QGroupBox::toggled, this, &MasterPanel::edited);
    for (QDoubleSpinBox* box : {ceiling_, release_})
        connect(box, &QDoubleSpinBox::valueChanged, this, &MasterPanel::edited);
    connect(target_, &QDoubleSpinBox::valueChanged, this, [this] {
        syncPreset();
        edited();
    });
    connect(preset_, &QComboBox::currentIndexChanged, this, &MasterPanel::presetChosen);
    connect(match_, &QPushButton::clicked, this, [this] { Q_EMIT matchRequested(target_->value()); });
    setSettings({}, 0.0);
}

void MasterPanel::setSettings(const daw::MasterSettings& settings, double masterGainDb)
{
    settings_ = settings;
    updating_ = true;
    const QSignalBlocker b1(limiter_), b2(ceiling_), b3(release_), b4(target_), b5(preset_);
    limiter_->setChecked(settings.limiter);
    ceiling_->setValue(settings.ceilingDbtp);
    release_->setValue(settings.releaseMs);
    target_->setValue(settings.targetLufs);
    syncPreset();
    fader_->setText(QStringLiteral("%1 dB").arg(masterGainDb, 0, 'f', 1));
    updating_ = false;
}

void MasterPanel::setLimiterReduction(float db)
{
    meter_->setReduction(settings_.limiter ? db : 0.0f);
}

void MasterPanel::setMatching(bool running)
{
    match_->setEnabled(!running);
    if (running)
        result_->setText(tr("Measuring the mix..."));
}

void MasterPanel::showMatch(const daw::LoudnessMatch& match, double targetLufs)
{
    QString text = tr("The mix measures %1 (true peak %2 dBTP). ").arg(lufs(match.mixLufs)).arg(match.mixTruePeakDbtp, 0, 'f', 1);
    if (match.reached)
        text += tr("With the master fader at %1 dB the master measures %2, true peak %3 dBTP; the limiter turns the loudest "
                   "peaks down by up to %4 dB.")
                    .arg(match.masterGainDb, 0, 'f', 1)
                    .arg(lufs(match.resultLufs))
                    .arg(match.resultTruePeakDbtp, 0, 'f', 1)
                    .arg(-match.maxReductionDb, 0, 'f', 1);
    else if (match.limited)
        text += tr("%1 would take more than %2 dB of limiting, which crushes the mix. The loudest clean setting, %3 dB on the "
                   "master fader, gives %4.")
                    .arg(lufs(targetLufs))
                    .arg(daw::kMaxLimiterReductionDb, 0, 'f', 0)
                    .arg(match.masterGainDb, 0, 'f', 1)
                    .arg(lufs(match.resultLufs));
    else
        text += tr("The closest setting, %1 dB on the master fader, gives %2.").arg(match.masterGainDb, 0, 'f', 1).arg(lufs(match.resultLufs));
    result_->setText(text);
}

void MasterPanel::showMatchError(const QString& message)
{
    result_->setText(message);
}

void MasterPanel::edited()
{
    if (updating_)
        return;
    settings_ = readControls();
    Q_EMIT settingsEdited(settings_);
}

void MasterPanel::presetChosen(int index)
{
    if (updating_ || index < 0 || index >= kCustomPreset)
        return;
    const daw::LoudnessPreset& p = daw::kLoudnessPresets[index];
    const QSignalBlocker b1(target_), b2(ceiling_);
    target_->setValue(p.lufs);
    ceiling_->setValue(p.ceilingDbtp);
    edited();
}

void MasterPanel::syncPreset()
{
    int index = kCustomPreset;
    for (int i = 0; i < kCustomPreset; ++i)
        if (std::abs(daw::kLoudnessPresets[i].lufs - target_->value()) < 0.05)
            index = i;
    const QSignalBlocker block(preset_);
    preset_->setCurrentIndex(index);
}

daw::MasterSettings MasterPanel::readControls() const
{
    daw::MasterSettings s = settings_;
    s.limiter = limiter_->isChecked();
    s.ceilingDbtp = ceiling_->value();
    s.releaseMs = release_->value();
    s.targetLufs = target_->value();
    return s;
}
