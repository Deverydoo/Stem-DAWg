// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "MasterPanel.h"

#include "HelpText.h"
#include "ReductionMeter.h"
#include "TonalChart.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
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

const char* const kEqNames[daw::MasterSettings::kEqBands] = {
    QT_TRANSLATE_NOOP("MasterPanel", "Low (100 Hz shelf)"), QT_TRANSLATE_NOOP("MasterPanel", "Low mids (250 Hz)"),
    QT_TRANSLATE_NOOP("MasterPanel", "Mids (1 kHz)"), QT_TRANSLATE_NOOP("MasterPanel", "Presence (3 kHz)"),
    QT_TRANSLATE_NOOP("MasterPanel", "Air (8 kHz shelf)")};

// Plain-language regions for describing a comparison.
struct Region {
    const char* name;
    double lowHz, highHz;
};
constexpr Region kRegions[] = {{QT_TRANSLATE_NOOP("MasterPanel", "sub-bass"), 30.0, 55.0},
                               {QT_TRANSLATE_NOOP("MasterPanel", "low end"), 55.0, 140.0},
                               {QT_TRANSLATE_NOOP("MasterPanel", "low mids"), 140.0, 450.0},
                               {QT_TRANSLATE_NOOP("MasterPanel", "mids"), 450.0, 1400.0},
                               {QT_TRANSLATE_NOOP("MasterPanel", "presence"), 1400.0, 4500.0},
                               {QT_TRANSLATE_NOOP("MasterPanel", "brilliance"), 4500.0, 9000.0},
                               {QT_TRANSLATE_NOOP("MasterPanel", "air"), 9000.0, 18000.0}};
constexpr double kMentionDb = 1.5;

} // namespace

MasterPanel::MasterPanel(QWidget* parent) : QWidget(parent)
{
    // Tonal EQ.
    eq_ = new QGroupBox(tr("EQ"), this);
    eq_->setCheckable(true);
    eq_->setToolTip(helpTip(tr("Master EQ"), tr("Five broad bands for the tonal balance of the whole mix, before the limiter.\n"
                                                "Compare (below) suggests settings from your reference tracks.")));
    auto* eqForm = new QFormLayout(eq_);
    for (std::size_t b = 0; b < eqGains_.size(); ++b) {
        auto* box = new QDoubleSpinBox(eq_);
        box->setRange(-daw::MasterSettings::kMaxEqDb, daw::MasterSettings::kMaxEqDb);
        box->setDecimals(1);
        box->setSingleStep(0.5);
        box->setSuffix(tr(" dB"));
        box->setKeyboardTracking(false);
        eqGains_[b] = box;
        eqForm->addRow(tr(kEqNames[b]), box);
    }

    // Limiter.
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

    // Loudness.
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

    // Tonal balance.
    auto* tonal = new QGroupBox(tr("Tonal balance"), this);
    tonal->setToolTip(helpTip(tr("Tonal balance"),
                              tr("Compares how the mix's energy spreads over frequency with reference tracks you choose:\n"
                                 "commercial releases in the genre you are aiming for. No genre curves are built in.")));
    tonalTarget_ = new QComboBox(tonal);
    tonalTarget_->setToolTip(helpTip(tr("Tonal target"), tr("The reference tracks the mix is compared with. New Target builds one\n"
                                                            "from a folder of tracks; it is saved for every project.")));
    newTarget_ = new QPushButton(tr("New Target..."), tonal);
    newTarget_->setToolTip(helpTip(tr("New target"), tr("Pick a folder of reference tracks (WAV, FLAC, MP3...). Each is measured and the\n"
                                                        "target is their average; how much they disagree sets how close the mix must be.")));
    compare_ = new QPushButton(tr("Compare"), tonal);
    compare_->setToolTip(helpTip(tr("Compare"), tr("Measures the mix as you hear it (master EQ included) against the target.")));
    chart_ = new TonalChart(tonal);
    chart_->setToolTip(helpTip(tr("Tonal balance"), tr("Bars: how far the mix is from the target per third of an octave, 31 Hz to 16 kHz.\n"
                                                       "Grey: how much your references disagree. Orange bars are outside that range.")));
    tonalText_ = new QLabel(tonal);
    tonalText_->setWordWrap(true);
    tonalText_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    applyEq_ = new QPushButton(tr("Apply Suggested EQ"), tonal);
    applyEq_->setEnabled(false);
    auto* targetRow = new QHBoxLayout;
    targetRow->addWidget(tonalTarget_, 1);
    targetRow->addWidget(newTarget_);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(compare_);
    buttons->addWidget(applyEq_);
    auto* tonalLayout = new QVBoxLayout(tonal);
    tonalLayout->addLayout(targetRow);
    tonalLayout->addWidget(chart_);
    tonalLayout->addWidget(tonalText_);
    tonalLayout->addLayout(buttons);

    auto* hint = new QLabel(tr("The master bus runs after the tracks and their clean-up, on everything you hear and export."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #7d8088;"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tonal);
    layout->addWidget(eq_);
    layout->addWidget(limiter_);
    layout->addWidget(loudness);
    layout->addWidget(hint);
    layout->addStretch(1);

    connect(eq_, &QGroupBox::toggled, this, &MasterPanel::edited);
    connect(limiter_, &QGroupBox::toggled, this, &MasterPanel::edited);
    for (QDoubleSpinBox* box : eqGains_)
        connect(box, &QDoubleSpinBox::valueChanged, this, &MasterPanel::edited);
    for (QDoubleSpinBox* box : {ceiling_, release_})
        connect(box, &QDoubleSpinBox::valueChanged, this, &MasterPanel::edited);
    connect(target_, &QDoubleSpinBox::valueChanged, this, [this] {
        syncPreset();
        edited();
    });
    connect(preset_, &QComboBox::currentIndexChanged, this, &MasterPanel::presetChosen);
    connect(match_, &QPushButton::clicked, this, [this] { Q_EMIT matchRequested(target_->value()); });
    connect(tonalTarget_, &QComboBox::currentIndexChanged, this, &MasterPanel::targetChosen);
    connect(newTarget_, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder of Reference Tracks"));
        if (!folder.isEmpty())
            Q_EMIT newTargetRequested(folder);
    });
    connect(compare_, &QPushButton::clicked, this, [this] { Q_EMIT compareRequested(); });
    connect(applyEq_, &QPushButton::clicked, this, [this] {
        if (!suggestedEq_)
            return;
        daw::MasterSettings s = readControls();
        s.eq = true;
        for (std::size_t b = 0; b < s.eqGainDb.size(); ++b)
            s.eqGainDb[b] = (*suggestedEq_)[b];
        settings_ = s;
        applyEq_->setEnabled(false);
        tonalText_->setText(tonalText_->text() + QStringLiteral(" ") + tr("Applied: click Compare to measure again."));
        Q_EMIT settingsEdited(settings_);
    });
    setSettings({}, 0.0);
}

void MasterPanel::setSettings(const daw::MasterSettings& settings, double masterGainDb)
{
    settings_ = settings;
    updating_ = true;
    const QSignalBlocker b1(limiter_), b2(ceiling_), b3(release_), b4(target_), b5(preset_), b6(eq_);
    eq_->setChecked(settings.eq);
    for (std::size_t b = 0; b < eqGains_.size(); ++b) {
        const QSignalBlocker block(eqGains_[b]);
        eqGains_[b]->setValue(settings.eqGainDb[b]);
    }
    limiter_->setChecked(settings.limiter);
    ceiling_->setValue(settings.ceilingDbtp);
    release_->setValue(settings.releaseMs);
    target_->setValue(settings.targetLufs);
    syncPreset();
    syncTarget();
    fader_->setText(QStringLiteral("%1 dB").arg(masterGainDb, 0, 'f', 1));
    updating_ = false;
}

void MasterPanel::setTargetLibrary(const std::vector<daw::TonalTargetSettings>& targets)
{
    library_ = targets;
    syncTarget();
}

void MasterPanel::syncTarget()
{
    // None, the library, and the project's own target if the library does not have it.
    const QSignalBlocker block(tonalTarget_);
    tonalTarget_->clear();
    tonalTarget_->addItem(tr("No target"));
    int current = 0;
    bool listed = false;
    for (std::size_t i = 0; i < library_.size(); ++i) {
        const auto& t = library_[i];
        tonalTarget_->addItem(tr("%1 (%n track(s))", nullptr, static_cast<int>(t.references.size())).arg(QString::fromStdString(t.name)));
        if (!settings_.tonalTarget.empty() && t == settings_.tonalTarget) {
            current = static_cast<int>(i) + 1;
            listed = true;
        }
    }
    if (!settings_.tonalTarget.empty() && !listed) {
        tonalTarget_->addItem(tr("%1 (this project)").arg(QString::fromStdString(settings_.tonalTarget.name)));
        current = tonalTarget_->count() - 1;
    }
    tonalTarget_->setCurrentIndex(current);
    compare_->setEnabled(!settings_.tonalTarget.empty() && newTarget_->isEnabled());
}

void MasterPanel::targetChosen(int index)
{
    if (updating_ || index < 0)
        return;
    daw::MasterSettings s = readControls();
    if (index == 0)
        s.tonalTarget = {};
    else if (index <= static_cast<int>(library_.size()))
        s.tonalTarget = library_[static_cast<std::size_t>(index - 1)];
    else
        return; // the project's own target, already chosen
    settings_ = s;
    chart_->clear();
    tonalText_->clear();
    suggestedEq_.reset();
    applyEq_->setEnabled(false);
    compare_->setEnabled(!s.tonalTarget.empty());
    Q_EMIT settingsEdited(settings_);
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

void MasterPanel::setTonalBusy(bool running, const QString& message)
{
    newTarget_->setEnabled(!running);
    compare_->setEnabled(!running && !settings_.tonalTarget.empty());
    if (running) {
        applyEq_->setEnabled(false);
        tonalText_->setText(message);
    }
}

void MasterPanel::showComparison(const daw::TonalComparison& c, const daw::TonalTargetSettings& target)
{
    std::array<double, daw::kTonalBands> spread{};
    for (std::size_t b = 0; b < spread.size() && b < target.spreadDb.size(); ++b)
        spread[b] = target.spreadDb[b];
    chart_->setComparison(c, spread);

    QStringList differences;
    for (const Region& r : kRegions) {
        double sum = 0.0;
        int count = 0;
        for (std::size_t b = 0; b < daw::kTonalBands; ++b)
            if (daw::kTonalBandHz[b] >= r.lowHz && daw::kTonalBandHz[b] < r.highHz) {
                sum += c.deviationDb[b];
                ++count;
            }
        const double mean = count > 0 ? sum / count : 0.0;
        if (std::abs(mean) >= kMentionDb)
            differences << (mean > 0.0 ? tr("the %1 %2 dB stronger") : tr("the %1 %2 dB weaker")).arg(tr(r.name)).arg(std::abs(mean), 0, 'f', 1);
    }
    QString text = tr("Against %1 (%n track(s)): ", nullptr, static_cast<int>(target.references.size())).arg(QString::fromStdString(target.name));
    text += differences.isEmpty() ? tr("the mix sits within the references' range.") : differences.join(QStringLiteral(", ")) + QStringLiteral(".");

    QStringList moves;
    for (std::size_t b = 0; b < c.eqGainDb.size(); ++b)
        if (c.eqGainDb[b] != 0.0f)
            moves << QStringLiteral("%1 %2 dB").arg(tr(kEqNames[b])).arg(static_cast<double>(c.eqGainDb[b]), 0, 'f', 1);
    bool changes = false;
    for (std::size_t b = 0; b < c.eqGainDb.size(); ++b)
        changes = changes || c.eqGainDb[b] != static_cast<float>(settings_.eq ? settings_.eqGainDb[b] : 0.0);
    if (changes && !moves.isEmpty())
        text += QStringLiteral(" ") + tr("Suggested master EQ: %1 (the difference falls from %2 to %3 dB).")
                                          .arg(moves.join(QStringLiteral(", ")))
                                          .arg(c.rmsBeforeDb, 0, 'f', 1)
                                          .arg(c.rmsAfterDb, 0, 'f', 1);
    else if (!changes)
        text += QStringLiteral(" ") + tr("The master EQ is already where the comparison would put it.");
    tonalText_->setText(text);
    suggestedEq_ = c.eqGainDb;
    applyEq_->setEnabled(changes);
}

void MasterPanel::showTonalError(const QString& message)
{
    tonalText_->setText(message);
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
    s.eq = eq_->isChecked();
    for (std::size_t b = 0; b < eqGains_.size(); ++b)
        s.eqGainDb[b] = eqGains_[b]->value();
    s.limiter = limiter_->isChecked();
    s.ceilingDbtp = ceiling_->value();
    s.releaseMs = release_->value();
    s.targetLufs = target_->value();
    return s;
}
