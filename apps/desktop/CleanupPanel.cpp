// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "CleanupPanel.h"

#include <daw/session/BleedTools.h>

#include "HelpText.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kFrequencySteps = 1000;
constexpr double kMeterRangeDb = 30.0;
constexpr double kExpanderMarginDb = 10.0; // same margin the stem report suggests

int frequencyToPosition(double hz)
{
    const double t = std::log(hz / daw::HighPassSettings::kMinHz)
                     / std::log(daw::HighPassSettings::kMaxHz / daw::HighPassSettings::kMinHz);
    return static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * kFrequencySteps));
}

double positionToFrequency(int position)
{
    const double t = static_cast<double>(position) / kFrequencySteps;
    return std::round(daw::HighPassSettings::kMinHz
                      * std::pow(daw::HighPassSettings::kMaxHz / daw::HighPassSettings::kMinHz, t));
}

QIcon swatch(const QColor& colour)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(colour.isValid() ? colour : QColor(Qt::gray));
    return QIcon(pixmap);
}

} // namespace

/// Horizontal gain-reduction bar: grows to the left from 0 dB.
class ReductionMeter final : public QWidget {
public:
    explicit ReductionMeter(QWidget* parent) : QWidget(parent) { setFixedHeight(16); }

    void setReduction(float db)
    {
        // Show the deepest of the new reading and the decaying old one.
        shown_ = std::min(db, std::min(0.0f, shown_ + 1.5f));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(0, 0, -1, -1);
        p.fillRect(r, QColor(0x22, 0x24, 0x28));
        const double fraction = std::clamp(-static_cast<double>(shown_) / kMeterRangeDb, 0.0, 1.0);
        const int w = static_cast<int>(std::lround(fraction * r.width()));
        p.fillRect(QRect(r.right() - w, r.top(), w, r.height()), QColor(0x4a, 0x90, 0xd9));
        p.setPen(QColor(0x3a, 0x3d, 0x44));
        p.drawRect(r);
        p.setPen(QColor(0xc8, 0xc8, 0xc8));
        p.drawText(r.adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   shown_ < -0.05f ? QStringLiteral("%1 dB").arg(static_cast<double>(shown_), 0, 'f', 1) : QStringLiteral("0 dB"));
    }

private:
    float shown_ = 0.0f;
};

CleanupPanel::CleanupPanel(QWidget* parent) : QWidget(parent)
{
    track_ = new QComboBox(this);
    track_->setToolTip(helpTip(tr("Track"), tr("The track whose clean-up chain is shown. The C button on a track header opens it here too.")));

    bypass_ = new QCheckBox(tr("Bypass"), this);
    bypass_->setToolTip(helpTip(tr("Bypass"), tr("Plays this track without its clean-up but keeps the settings.\n"
                                                  "Bypass All Clean-up (Ctrl+B) compares every track at once.")));
    reset_ = new QPushButton(tr("Reset"), this);
    reset_->setToolTip(helpTip(tr("Reset"), tr("Switches every stage off and restores the default settings for this track.")));

    // Bleed suppression (stems): rendered against every other stem, not live.
    bleed_ = new QGroupBox(tr("Bleed suppression (stems)"), this);
    bleed_->setToolTip(helpTip(tr("Bleed suppression"),
                               tr("Compares every stem with all the others and turns each one down wherever\n"
                                  "another stem owns the sound: the hi-hat ghosting through the vocal, the kick\n"
                                  "inside the bass. It renders a cleaned copy of the stem, which the track then\n"
                                  "plays; the original stays in the project (Original switches back).")));
    bleedState_ = new QLabel(bleed_);
    bleedState_->setWordWrap(true);
    bleedState_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    bleedStrength_ = new QComboBox(bleed_);
    for (const daw::BleedStrength& s : daw::kBleedStrengths)
        bleedStrength_->addItem(tr(s.name), static_cast<double>(s.reductionDb));
    bleedStrength_->setCurrentIndex(1);
    bleedStrength_->setToolTip(helpTip(tr("Strength"), tr("Light keeps more of what stems share;\n"
                                                          "Strong separates hardest (up to 24 dB), and can thin\n"
                                                          "parts that several stems play together.")));
    bleedApply_ = new QPushButton(tr("This Track"), bleed_);
    bleedApply_->setToolTip(helpTip(tr("Suppress bleed in this track"),
                                    tr("Renders this stem, judged against every other stem\n"
                                       "(a few seconds per song). Ctrl+Z undoes it.")));
    bleedApplyAll_ = new QPushButton(tr("All Stems"), bleed_);
    bleedApplyAll_->setToolTip(helpTip(tr("Suppress bleed in every stem"),
                                       tr("Renders every stem that plays recorded audio, in one undo step.")));
    bleedRevert_ = new QPushButton(tr("Original"), bleed_);
    bleedRevert_->setToolTip(helpTip(tr("Original recording"), tr("The track plays its original recording again.")));
    // Two rows, so the panel stays narrow.
    auto* bleedRow = new QHBoxLayout;
    bleedRow->addWidget(new QLabel(tr("Strength"), bleed_));
    bleedRow->addWidget(bleedStrength_);
    bleedRow->addStretch(1);
    bleedRow->addWidget(bleedRevert_);
    auto* bleedButtons = new QHBoxLayout;
    bleedButtons->addStretch(1);
    bleedButtons->addWidget(bleedApply_);
    bleedButtons->addWidget(bleedApplyAll_);
    auto* bleedLayout = new QVBoxLayout(bleed_);
    bleedLayout->addWidget(bleedState_);
    bleedLayout->addLayout(bleedRow);
    bleedLayout->addLayout(bleedButtons);

    // Voice denoise (speech): rendered by the neural model, not live.
    voice_ = new QGroupBox(tr("Voice denoise (speech, AI)"), this);
    voice_->setToolTip(helpTip(tr("Voice denoise"), tr("A neural network (DeepFilterNet3) separates speech from any noise:\n"
                                                       "fans, traffic, keyboards, room tone, even noise that keeps changing.\n"
                                                       "It renders a cleaned copy of the recording, which the track then plays;\n"
                                                       "the original stays in the project (Original switches back).\n"
                                                       "Made for speech: a sung voice can be taken for noise.")));
    voiceState_ = new QLabel(voice_);
    voiceState_->setWordWrap(true);
    voiceState_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    voiceReduction_ = new QComboBox(voice_);
    for (const int db : {6, 12, 18, 24})
        voiceReduction_->addItem(tr("%1 dB").arg(db), static_cast<double>(db));
    voiceReduction_->addItem(tr("Full"), 100.0);
    voiceReduction_->setCurrentIndex(voiceReduction_->count() - 1);
    voiceReduction_->setToolTip(helpTip(tr("Reduction"), tr("The most the noise is turned down. Full removes all it can;\n"
                                                             "12-18 dB keeps a little room sound and is gentler on the voice.")));
    voiceApply_ = new QPushButton(tr("Apply"), voice_);
    voiceApply_->setToolTip(helpTip(tr("Apply voice denoise"), tr("Renders the track's original recording through the model\n"
                                                                   "(a few seconds per minute of audio). Ctrl+Z undoes it.")));
    voiceRevert_ = new QPushButton(tr("Original"), voice_);
    voiceRevert_->setToolTip(helpTip(tr("Original recording"), tr("The track plays its original recording again.")));
    auto* voiceRow = new QHBoxLayout;
    voiceRow->addWidget(new QLabel(tr("Reduction"), voice_));
    voiceRow->addWidget(voiceReduction_);
    voiceRow->addStretch(1);
    voiceRow->addWidget(voiceRevert_);
    auto* voiceButtons = new QHBoxLayout;
    voiceButtons->addStretch(1);
    voiceButtons->addWidget(voiceApply_);
    auto* voiceLayout = new QVBoxLayout(voice_);
    voiceLayout->addWidget(voiceState_);
    voiceLayout->addLayout(voiceRow);
    voiceLayout->addLayout(voiceButtons);

    dc_ = new QCheckBox(tr("DC removal"), this);
    dc_->setToolTip(helpTip(tr("DC removal"), tr("Removes a constant offset from the waveform (a 5 Hz high-pass).\n"
                                                  "An offset wastes headroom and clicks at edits. Harmless to leave on.")));

    // High-pass.
    highPass_ = new QGroupBox(tr("High-pass"), this);
    highPass_->setCheckable(true);
    highPass_->setToolTip(helpTip(tr("High-pass filter"), tr("Removes everything below the frequency: rumble, and low end that belongs to another stem.\n"
                                                             "Only kick and bass should own the sub; high-pass the rest around 80-150 Hz.")));
    hpSlider_ = new QSlider(Qt::Horizontal, highPass_);
    hpSlider_->setRange(0, kFrequencySteps);
    hpFrequency_ = new QDoubleSpinBox(highPass_);
    hpFrequency_->setRange(daw::HighPassSettings::kMinHz, daw::HighPassSettings::kMaxHz);
    hpFrequency_->setDecimals(0);
    hpFrequency_->setSuffix(tr(" Hz"));
    hpFrequency_->setKeyboardTracking(false);
    hpFrequency_->setToolTip(helpTip(tr("Frequency"), tr("Where the filter starts cutting (-3 dB point).")));
    hpSlope_ = new QComboBox(highPass_);
    hpSlope_->addItem(tr("12 dB/octave (gentle)"), 12);
    hpSlope_->addItem(tr("24 dB/octave"), 24);
    hpSlope_->addItem(tr("48 dB/octave (steep)"), 48);
    hpSlope_->setToolTip(helpTip(tr("Slope"), tr("How quickly the level falls below the frequency. Steeper keeps more of what is above it.")));
    auto* hpRow = new QHBoxLayout;
    hpRow->addWidget(hpSlider_, 1);
    hpRow->addWidget(hpFrequency_);
    auto* hpForm = new QFormLayout(highPass_);
    hpForm->addRow(tr("Frequency"), hpRow);
    hpForm->addRow(tr("Slope"), hpSlope_);

    // Hum removal.
    hum_ = new QGroupBox(tr("Hum removal"), this);
    hum_->setCheckable(true);
    hum_->setToolTip(helpTip(tr("Hum removal"), tr("Narrow notches at the mains frequency and its harmonics: the buzz of guitar\n"
                                                    "amps, cheap cables and ground loops. 50 Hz in Europe and most of the world,\n"
                                                    "60 Hz in the Americas. Detect finds it for you.")));
    humMains_ = new QComboBox(hum_);
    humMains_->addItem(tr("50 Hz"), 50.0);
    humMains_->addItem(tr("60 Hz"), 60.0);
    humMains_->setToolTip(helpTip(tr("Mains frequency"), tr("The hum's fundamental.")));
    humHarmonics_ = new QSpinBox(hum_);
    humHarmonics_->setRange(1, 8);
    humHarmonics_->setKeyboardTracking(false);
    humHarmonics_->setToolTip(helpTip(tr("Harmonics"), tr("How many notches: at the fundamental, twice it, three times it...\n"
                                                           "Buzzy hum needs more. Each notch is 3 Hz wide, so music barely notices.")));
    detectHum_ = new QPushButton(tr("Detect"), hum_);
    detectHum_->setToolTip(helpTip(tr("Detect hum"), tr("Looks for 50 or 60 Hz hum in the quiet parts of the track and,\n"
                                                         "when it finds some, sets the frequency and harmonics and switches this on.\n"
                                                         "With a time selection on the timeline, it looks in the selection only.")));
    auto* humRow = new QHBoxLayout;
    humRow->addWidget(humMains_);
    humRow->addWidget(new QLabel(tr("Harmonics"), hum_));
    humRow->addWidget(humHarmonics_);
    humRow->addStretch(1);
    humRow->addWidget(detectHum_);
    hum_->setLayout(humRow);

    // Noise reduction.
    denoise_ = new QGroupBox(tr("Noise reduction"), this);
    denoise_->setCheckable(true);
    denoise_->setToolTip(helpTip(tr("Noise reduction"), tr("Removes steady noise (fans, air conditioning, hiss, the bed of an AI stem)\n"
                                                            "using a profile of the noise learned from the track's quietest moments.\n"
                                                            "Click Learn Noise first. Listen to Removed checks that no music goes with it.")));
    learn_ = new QPushButton(tr("Learn Noise"), denoise_);
    learn_->setToolTip(helpTip(tr("Learn noise"), tr("Measures the noise in the quietest 10% of the track (silence is ignored)\n"
                                                      "and switches noise reduction on. Learn again after changing the high-pass or hum removal.\n"
                                                      "For noise the quietest moments miss, drag across a stretch of pure noise on\n"
                                                      "the timeline first: it then learns from all of the selection.")));
    profile_ = new QLabel(denoise_);
    profile_->setWordWrap(true);
    profile_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    reductionSlider_ = new QSlider(Qt::Horizontal, denoise_);
    reductionSlider_->setRange(0, 40);
    reduction_ = new QDoubleSpinBox(denoise_);
    reduction_->setRange(0.0, 40.0);
    reduction_->setDecimals(0);
    reduction_->setSuffix(tr(" dB"));
    reduction_->setKeyboardTracking(false);
    reduction_->setToolTip(helpTip(tr("Reduction"), tr("How far the noise is turned down. 10-15 dB sounds natural;\n"
                                                        "more removes more but can leave a watery, \"underwater\" sound.")));
    sensitivity_ = new QDoubleSpinBox(denoise_);
    sensitivity_->setRange(-10.0, 10.0);
    sensitivity_->setDecimals(1);
    sensitivity_->setSingleStep(1.0);
    sensitivity_->setSuffix(tr(" dB"));
    sensitivity_->setKeyboardTracking(false);
    sensitivity_->setToolTip(helpTip(tr("Sensitivity"), tr("Treats the noise as this much louder than learned. Raise it when noise is left over,\n"
                                                            "lower it when quiet music starts to disappear.")));
    auto* reductionRow = new QHBoxLayout;
    reductionRow->addWidget(reductionSlider_, 1);
    reductionRow->addWidget(reduction_);
    auto* learnRow = new QHBoxLayout;
    learnRow->addWidget(learn_);
    learnRow->addWidget(profile_, 1);
    auto* dnForm = new QFormLayout(denoise_);
    dnForm->addRow(learnRow);
    dnForm->addRow(tr("Reduction"), reductionRow);
    dnForm->addRow(tr("Sensitivity"), sensitivity_);

    // Expander.
    expander_ = new QGroupBox(tr("Expander"), this);
    expander_->setCheckable(true);
    expander_->setToolTip(helpTip(tr("Downward expander"), tr("Turns the track down when it falls below the threshold, so the noise bed\n"
                                                              "between notes disappears while the notes play untouched.\n"
                                                              "It looks 5 ms ahead, so it is already open when a note starts.")));
    thresholdSlider_ = new QSlider(Qt::Horizontal, expander_);
    thresholdSlider_->setRange(-90, 0);
    threshold_ = new QDoubleSpinBox(expander_);
    threshold_->setRange(-90.0, 0.0);
    threshold_->setDecimals(1);
    threshold_->setSuffix(tr(" dB"));
    threshold_->setKeyboardTracking(false);
    threshold_->setToolTip(helpTip(tr("Threshold"), tr("Level (RMS) below which the track is turned down.\n"
                                                        "Set it a few dB above the noise bed: the stem report measures the bed.")));
    ratio_ = new QDoubleSpinBox(expander_);
    ratio_->setRange(1.0, 20.0);
    ratio_->setDecimals(1);
    ratio_->setSingleStep(0.5);
    ratio_->setPrefix(tr("1:"));
    ratio_->setKeyboardTracking(false);
    ratio_->setToolTip(helpTip(tr("Ratio"), tr("How hard it turns down: at 1:4, 1 dB below the threshold becomes 4 dB lower.\n"
                                                "1:1 does nothing; high ratios act like a noise gate.")));
    range_ = new QDoubleSpinBox(expander_);
    range_->setRange(0.0, 90.0);
    range_->setDecimals(0);
    range_->setSuffix(tr(" dB"));
    range_->setKeyboardTracking(false);
    range_->setToolTip(helpTip(tr("Range"), tr("The most it ever turns down. 20-30 dB hides a noise bed and keeps quiet tails natural;\n"
                                                "higher values silence the gaps completely.")));
    release_ = new QDoubleSpinBox(expander_);
    release_->setRange(10.0, 2000.0);
    release_->setDecimals(0);
    release_->setSingleStep(10.0);
    release_->setSuffix(tr(" ms"));
    release_->setKeyboardTracking(false);
    release_->setToolTip(helpTip(tr("Release"), tr("How slowly it turns down after a note ends (after a 20 ms hold).\n"
                                                    "Longer keeps reverb tails; shorter cleans faster.")));
    meter_ = new ReductionMeter(expander_);
    meter_->setToolTip(helpTip(tr("Gain reduction"), tr("How far the expander is turning the track down right now (during playback).")));
    floor_ = new QLabel(expander_);
    floor_->setWordWrap(true);
    floor_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    fromFloor_ = new QPushButton(tr("Set from Noise Bed"), expander_);
    fromFloor_->setToolTip(helpTip(tr("Set from noise bed"), tr("Puts the threshold 10 dB above the noise bed the stem report measured,\n"
                                                                "with a ratio that turns the bed down by the full range.")));
    auto* thresholdRow = new QHBoxLayout;
    thresholdRow->addWidget(thresholdSlider_, 1);
    thresholdRow->addWidget(threshold_);
    auto* exForm = new QFormLayout(expander_);
    exForm->addRow(tr("Threshold"), thresholdRow);
    exForm->addRow(tr("Ratio"), ratio_);
    exForm->addRow(tr("Range"), range_);
    exForm->addRow(tr("Release"), release_);
    exForm->addRow(tr("Reduction"), meter_);
    exForm->addRow(floor_);
    exForm->addRow(fromFloor_);

    // Stereo repair.
    stereo_ = new QGroupBox(tr("Stereo"), this);
    stereo_->setCheckable(true);
    stereo_->setToolTip(helpTip(tr("Stereo repair"),
                                tr("Fixes the stereo image of AI stems: a low end that is wide or out of phase, and sound\n"
                                   "that cancels when the song is summed to mono (phones, Bluetooth speakers, clubs).")));
    monoBass_ = new QCheckBox(tr("Mono below"), stereo_);
    monoBass_->setToolTip(helpTip(tr("Mono bass"), tr("Everything below this frequency becomes mono. Separated stems often carry a wide\n"
                                                      "or out-of-phase low end; a mono low end keeps its punch on every system.")));
    monoBelow_ = new QDoubleSpinBox(stereo_);
    monoBelow_->setRange(daw::StereoSettings::kMinHz, daw::StereoSettings::kMaxHz);
    monoBelow_->setDecimals(0);
    monoBelow_->setSingleStep(10.0);
    monoBelow_->setSuffix(tr(" Hz"));
    monoBelow_->setKeyboardTracking(false);
    monoBelow_->setToolTip(monoBass_->toolTip());
    width_ = new QDoubleSpinBox(stereo_);
    width_->setRange(0.0, daw::StereoSettings::kMaxWidthPercent);
    width_->setDecimals(0);
    width_->setSingleStep(5.0);
    width_->setSuffix(tr(" %"));
    width_->setKeyboardTracking(false);
    width_->setToolTip(helpTip(tr("Width"), tr("0 % is mono, 100 % leaves the image as it is, 200 % doubles the difference\n"
                                               "between the channels. Narrowing tames a phasey, swirling stem.")));
    phaseAlign_ = new QCheckBox(tr("Align phase (mono-safe)"), stereo_);
    phaseAlign_->setToolTip(helpTip(tr("Phase alignment"),
                                    tr("Where left and right carry the same sound out of step, brings them into step, so a\n"
                                       "mono sum keeps it. Each channel keeps its level; real width and reverb are left\n"
                                       "alone. The stem report says when it helps.")));
    auto* monoRow = new QHBoxLayout;
    monoRow->addWidget(monoBass_);
    monoRow->addWidget(monoBelow_, 1);
    auto* stForm = new QFormLayout(stereo_);
    stForm->addRow(monoRow);
    stForm->addRow(tr("Width"), width_);
    stForm->addRow(phaseAlign_);

    // Harshness and shimmer control.
    resonance_ = new QGroupBox(tr("Harshness"), this);
    resonance_->setCheckable(true);
    resonance_->setToolTip(helpTip(tr("Harshness and shimmer control"),
                                   tr("Turns down narrow peaks that keep standing above the sound around them, and only\n"
                                      "while they do: a partial that rings through many notes (harsh at 2-6 kHz), or a\n"
                                      "steady tone AI generators leave in the high end (often near 16 kHz, heard as a\n"
                                      "whine or shimmer). Notes that come and go are left alone. It reacts within about\n"
                                      "a second, so it follows the song rather than single notes.")));
    const auto depthBox = [this](const QString& title, const QString& help) {
        auto* box = new QDoubleSpinBox(resonance_);
        box->setRange(0.0, daw::ResonanceSettings::kMaxDb);
        box->setDecimals(1);
        box->setSingleStep(1.0);
        box->setSuffix(tr(" dB"));
        box->setSpecialValueText(tr("Off"));
        box->setKeyboardTracking(false);
        box->setToolTip(helpTip(title, help));
        return box;
    };
    harsh_ = depthBox(tr("2-6 kHz"), tr("The most a resonance between 2 and 6 kHz is turned down. A peak is only ever\n"
                                        "brought down towards the sound around it, never below. 6 dB suits most AI songs."));
    shimmer_ = depthBox(tr("8-17 kHz"), tr("The most a tone between 8 and 17.5 kHz is turned down. A peak is only ever\n"
                                           "brought down towards the sound around it, never below. 6 dB suits most AI songs."));
    auto* rsForm = new QFormLayout(resonance_);
    rsForm->addRow(tr("2-6 kHz"), harsh_);
    rsForm->addRow(tr("8-17 kHz"), shimmer_);

    // Transient shaper.
    transient_ = new QGroupBox(tr("Transients"), this);
    transient_->setCheckable(true);
    transient_->setToolTip(helpTip(tr("Transient shaper"), tr("Sharpens or softens the start of each hit and turns the decay after it up or down,\n"
                                                              "at any level. AI-generated drums take 2-4 ms to rise to their peak, where a\n"
                                                              "sampled hit takes about 1 ms (the stem report's Attack column): attack brings\n"
                                                              "the snap back.")));
    const auto shaperBox = [this](const QString& title, const QString& help) {
        auto* box = new QDoubleSpinBox(transient_);
        box->setRange(-daw::TransientSettings::kMaxDb, daw::TransientSettings::kMaxDb);
        box->setDecimals(1);
        box->setSingleStep(1.0);
        box->setSuffix(tr(" dB"));
        box->setKeyboardTracking(false);
        box->setToolTip(helpTip(title, help));
        return box;
    };
    attack_ = shaperBox(tr("Attack"), tr("Positive: each hit rises faster. The slow start of a hit (above 1 kHz) is lifted\n"
                                         "towards its peak, by at most this much; the peak level itself stays put.\n"
                                         "+6 dB suits soft AI drums. Negative: the start of each hit is turned down,\n"
                                         "softening hits that click."));
    sustain_ = shaperBox(tr("Sustain"), tr("Gain on the decay after each hit.\n"
                                           "Negative values tighten boomy or roomy drums; positive values bring tails up."));
    auto* trForm = new QFormLayout(transient_);
    trForm->addRow(tr("Attack"), attack_);
    trForm->addRow(tr("Sustain"), sustain_);

    auto* hint = new QLabel(tr("Bleed suppression and voice denoise render a copy; the rest is applied live, before the "
                               "fader, in this order. Ctrl+B compares with the original."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #7d8088;"));

    listen_ = new QToolButton(this);
    listen_->setVisible(false);

    editor_ = new QWidget(this);
    auto* top = new QHBoxLayout;
    top->addWidget(bypass_);
    top->addStretch(1);
    top->addWidget(listen_);
    top->addWidget(reset_);
    auto* editorLayout = new QVBoxLayout(editor_);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->addLayout(top);
    editorLayout->addWidget(bleed_);
    editorLayout->addWidget(voice_);
    editorLayout->addWidget(dc_);
    editorLayout->addWidget(highPass_);
    editorLayout->addWidget(stereo_);
    editorLayout->addWidget(hum_);
    editorLayout->addWidget(denoise_);
    editorLayout->addWidget(resonance_);
    editorLayout->addWidget(expander_);
    editorLayout->addWidget(transient_);
    editorLayout->addWidget(hint);
    editorLayout->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(editor_);

    empty_ = new QLabel(tr("Import stems to clean them up."), this);
    empty_->setStyleSheet(QStringLiteral("color: #9a9da5;"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->addWidget(track_);
    layout->addWidget(empty_);
    layout->addWidget(scroll, 1);
    scroll_ = scroll;

    connect(track_, &QComboBox::currentIndexChanged, this, [this] { showCurrent(); });
    connect(bypass_, &QCheckBox::toggled, this, &CleanupPanel::edited);
    connect(dc_, &QCheckBox::toggled, this, &CleanupPanel::edited);
    connect(highPass_, &QGroupBox::toggled, this, &CleanupPanel::edited);
    connect(expander_, &QGroupBox::toggled, this, &CleanupPanel::edited);
    connect(stereo_, &QGroupBox::toggled, this, &CleanupPanel::edited);
    connect(monoBass_, &QCheckBox::toggled, this, &CleanupPanel::edited);
    connect(phaseAlign_, &QCheckBox::toggled, this, &CleanupPanel::edited);
    connect(resonance_, &QGroupBox::toggled, this, &CleanupPanel::edited);
    connect(transient_, &QGroupBox::toggled, this, &CleanupPanel::edited);
    connect(hum_, &QGroupBox::toggled, this, &CleanupPanel::edited);
    connect(denoise_, &QGroupBox::toggled, this, &CleanupPanel::edited);
    // An unchecked group disables its children, but Detect and Learn Noise are
    // how a stage gets switched on: keep them usable (a disabled group, when
    // the track is bypassed, still disables them).
    const auto keepActionsEnabled = [this] {
        detectHum_->setEnabled(true);
        learn_->setEnabled(true);
    };
    connect(hum_, &QGroupBox::toggled, this, keepActionsEnabled);
    connect(denoise_, &QGroupBox::toggled, this, keepActionsEnabled);
    connect(humMains_, &QComboBox::currentIndexChanged, this, &CleanupPanel::edited);
    connect(humHarmonics_, &QSpinBox::valueChanged, this, &CleanupPanel::edited);
    connect(reduction_, &QDoubleSpinBox::valueChanged, this, &CleanupPanel::edited);
    connect(sensitivity_, &QDoubleSpinBox::valueChanged, this, &CleanupPanel::edited);
    connect(reductionSlider_, &QSlider::valueChanged, this, [this](int db) {
        if (updating_)
            return;
        const QSignalBlocker block(reduction_);
        reduction_->setValue(db);
        edited();
    });
    connect(learn_, &QPushButton::clicked, this, [this] {
        if (const CleanupTrackInfo* info = current())
            Q_EMIT learnNoiseRequested(info->id);
    });
    connect(detectHum_, &QPushButton::clicked, this, [this] {
        if (const CleanupTrackInfo* info = current())
            Q_EMIT detectHumRequested(info->id);
    });
    connect(voiceApply_, &QPushButton::clicked, this, [this] {
        if (const CleanupTrackInfo* info = current())
            Q_EMIT voiceDenoiseRequested(info->id, voiceReduction_->currentData().toDouble());
    });
    connect(voiceRevert_, &QPushButton::clicked, this, [this] {
        if (const CleanupTrackInfo* info = current())
            Q_EMIT revertRequested(info->id);
    });
    connect(bleedApply_, &QPushButton::clicked, this, [this] {
        if (const CleanupTrackInfo* info = current())
            Q_EMIT bleedRequested(info->id, bleedStrength_->currentData().toDouble());
    });
    connect(bleedApplyAll_, &QPushButton::clicked, this,
            [this] { Q_EMIT bleedRequested(QString(), bleedStrength_->currentData().toDouble()); });
    connect(bleedRevert_, &QPushButton::clicked, this, [this] {
        if (const CleanupTrackInfo* info = current())
            Q_EMIT revertRequested(info->id);
    });
    connect(hpSlope_, &QComboBox::currentIndexChanged, this, &CleanupPanel::edited);
    for (QDoubleSpinBox* box : {hpFrequency_, threshold_, ratio_, range_, release_, monoBelow_, width_, harsh_, shimmer_, attack_, sustain_})
        connect(box, &QDoubleSpinBox::valueChanged, this, &CleanupPanel::edited);
    connect(hpSlider_, &QSlider::valueChanged, this, [this](int position) {
        if (updating_)
            return;
        const QSignalBlocker block(hpFrequency_);
        hpFrequency_->setValue(positionToFrequency(position));
        edited();
    });
    connect(thresholdSlider_, &QSlider::valueChanged, this, [this](int db) {
        if (updating_)
            return;
        const QSignalBlocker block(threshold_);
        threshold_->setValue(db);
        edited();
    });
    connect(reset_, &QPushButton::clicked, this, [this] {
        if (const CleanupTrackInfo* info = current())
            Q_EMIT cleanupEdited(info->id, daw::TrackCleanup{});
    });
    connect(fromFloor_, &QPushButton::clicked, this, [this] {
        const CleanupTrackInfo* info = current();
        if (info == nullptr || !info->noiseFloorDbfs)
            return;
        daw::TrackCleanup cleanup = readControls();
        cleanup.bypassed = false;
        cleanup.expander.enabled = true;
        cleanup.expander.thresholdDb = std::round(*info->noiseFloorDbfs + kExpanderMarginDb);
        cleanup.expander.ratio = 1.0 + cleanup.expander.rangeDb / kExpanderMarginDb;
        cleanup.clamp();
        Q_EMIT cleanupEdited(info->id, cleanup);
    });

    showCurrent();
}

void CleanupPanel::setTracks(const std::vector<CleanupTrackInfo>& tracks)
{
    const QString selected = currentTrackId();
    bool sameList = tracks.size() == tracks_.size();
    for (std::size_t i = 0; sameList && i < tracks.size(); ++i)
        sameList = tracks[i].id == tracks_[i].id && tracks[i].name == tracks_[i].name && tracks[i].color == tracks_[i].color;
    tracks_ = tracks;

    if (!sameList) {
        const QSignalBlocker block(track_);
        track_->clear();
        for (const CleanupTrackInfo& t : tracks_)
            track_->addItem(swatch(t.color), t.name, t.id);
        const int index = track_->findData(selected);
        track_->setCurrentIndex(index >= 0 ? index : (tracks_.empty() ? -1 : 0));
    }
    showCurrent();
}

void CleanupPanel::selectTrack(const QString& id)
{
    const int index = track_->findData(id);
    if (index >= 0)
        track_->setCurrentIndex(index);
}

void CleanupPanel::setSelectionActive(bool active)
{
    learn_->setText(active ? tr("Learn from Selection") : tr("Learn Noise"));
    detectHum_->setText(active ? tr("Detect in Selection") : tr("Detect"));
}

QString CleanupPanel::currentTrackId() const
{
    return track_->currentData().toString();
}

void CleanupPanel::setGainReduction(float db)
{
    meter_->setReduction(db);
}

void CleanupPanel::setListenAction(QAction* action)
{
    listen_->setDefaultAction(action);
    listen_->setVisible(action != nullptr);
}

const CleanupTrackInfo* CleanupPanel::current() const
{
    const QString id = currentTrackId();
    for (const CleanupTrackInfo& t : tracks_)
        if (t.id == id)
            return &t;
    return nullptr;
}

void CleanupPanel::showCurrent()
{
    const CleanupTrackInfo* info = current();
    empty_->setVisible(info == nullptr);
    scroll_->setVisible(info != nullptr);
    track_->setVisible(info != nullptr);
    if (info == nullptr)
        return;

    updating_ = true;
    const daw::TrackCleanup& c = info->cleanup;
    const QSignalBlocker b1(bypass_), b2(dc_), b3(highPass_), b4(expander_), b5(hpSlope_), b6(hpFrequency_), b7(threshold_),
        b8(ratio_), b9(range_), b10(release_), b11(hum_), b12(humMains_), b13(humHarmonics_), b14(denoise_), b15(reduction_),
        b16(sensitivity_), b17(transient_), b18(attack_), b19(sustain_), b20(resonance_), b21(harsh_), b22(shimmer_), b23(stereo_), b24(monoBass_),
        b25(monoBelow_), b26(width_), b27(phaseAlign_);
    hum_->setChecked(c.hum.enabled);
    humMains_->setCurrentIndex(c.hum.frequencyHz < 55.0 ? 0 : 1);
    humHarmonics_->setValue(c.hum.harmonics);
    denoise_->setChecked(c.denoise.enabled);
    if (!reductionSlider_->isSliderDown())
        reductionSlider_->setValue(static_cast<int>(std::lround(c.denoise.reductionDb)));
    reduction_->setValue(c.denoise.reductionDb);
    sensitivity_->setValue(c.denoise.sensitivityDb);
    profile_->setText(c.denoise.profile.empty()
                          ? tr("No noise profile yet: click Learn Noise.")
                          : (c.denoise.profile.fromSelection ? tr("Learned from %1 s of a selection.")
                                                             : tr("Learned from %1 s of the quietest audio."))
                                .arg(c.denoise.profile.seconds, 0, 'f', 1));
    if (info->voiceDenoiseDb) {
        const double db = *info->voiceDenoiseDb;
        voiceState_->setText(db >= 100.0 ? tr("On: the track plays a denoised copy (full reduction).")
                                         : tr("On: the track plays a denoised copy (%1 dB reduction).").arg(db, 0, 'f', 0));
        const int index = voiceReduction_->findData(db);
        if (index >= 0)
            voiceReduction_->setCurrentIndex(index);
    } else {
        voiceState_->setText(info->bleedDb ? tr("Off.") : tr("Off: the track plays its original recording."));
    }
    voiceState_->setText(info->renderRunning ? tr("Rendering...") : voiceState_->text());
    voiceApply_->setText(info->voiceDenoiseDb ? tr("Re-apply") : tr("Apply"));
    voiceApply_->setEnabled(!info->renderRunning);
    voiceRevert_->setEnabled(info->voiceDenoiseDb.has_value() && !info->renderRunning);

    if (info->bleedDb) {
        const daw::BleedStrength& strength = daw::bleedStrengthFor(*info->bleedDb);
        bleedState_->setText(tr("On: the track plays a copy with the other stems' bleed turned down (%1).").arg(tr(strength.name)));
        const int index = bleedStrength_->findData(static_cast<double>(strength.reductionDb));
        if (index >= 0)
            bleedStrength_->setCurrentIndex(index);
    } else if (!info->bleedAvailable) {
        bleedState_->setText(tr("Needs at least two tracks of recorded audio (stems) to compare."));
    } else {
        bleedState_->setText(info->voiceDenoiseDb ? tr("Off.") : tr("Off: the track plays its original recording."));
    }
    bleedState_->setText(info->renderRunning ? tr("Rendering...") : bleedState_->text());
    bleedApply_->setEnabled(info->bleedAvailable && !info->renderRunning);
    bleedApplyAll_->setEnabled(info->bleedAvailable && !info->renderRunning);
    bleedRevert_->setEnabled(info->bleedDb.has_value() && !info->renderRunning);
    bypass_->setChecked(c.bypassed);
    dc_->setChecked(c.dcRemoval);
    highPass_->setChecked(c.highPass.enabled);
    if (!hpSlider_->isSliderDown())
        hpSlider_->setValue(frequencyToPosition(c.highPass.frequencyHz));
    hpFrequency_->setValue(c.highPass.frequencyHz);
    hpSlope_->setCurrentIndex(std::max(0, hpSlope_->findData(c.highPass.slopeDbPerOctave)));
    expander_->setChecked(c.expander.enabled);
    if (!thresholdSlider_->isSliderDown())
        thresholdSlider_->setValue(static_cast<int>(std::lround(c.expander.thresholdDb)));
    threshold_->setValue(c.expander.thresholdDb);
    ratio_->setValue(c.expander.ratio);
    range_->setValue(c.expander.rangeDb);
    release_->setValue(c.expander.releaseMs);
    stereo_->setChecked(c.stereo.enabled);
    monoBass_->setChecked(c.stereo.monoBass);
    monoBelow_->setValue(c.stereo.monoBelowHz);
    width_->setValue(c.stereo.widthPercent);
    phaseAlign_->setChecked(c.stereo.phaseAlign);
    resonance_->setChecked(c.resonance.enabled);
    harsh_->setValue(c.resonance.harshDb);
    shimmer_->setValue(c.resonance.shimmerDb);
    transient_->setChecked(c.transient.enabled);
    attack_->setValue(c.transient.attackDb);
    sustain_->setValue(c.transient.sustainDb);
    updating_ = false;

    for (QWidget* w : std::initializer_list<QWidget*>{dc_, highPass_, stereo_, hum_, denoise_, resonance_, expander_, transient_})
        w->setEnabled(!c.bypassed);
    detectHum_->setEnabled(true); // see keepActionsEnabled in the constructor
    learn_->setEnabled(true);
    if (info->noiseFloorDbfs) {
        floor_->setText(info->floorAfterExpander
                            ? tr("Noise bed after the expander: %1 dBFS (stem report).").arg(*info->noiseFloorDbfs, 0, 'f', 1)
                            : tr("Noise bed: %1 dBFS (stem report).").arg(*info->noiseFloorDbfs, 0, 'f', 1));
        fromFloor_->setEnabled(!info->floorAfterExpander);
    } else {
        floor_->setText(tr("No noise bed measured. Run the stem report (Ctrl+R) to measure one."));
        fromFloor_->setEnabled(false);
    }
}

daw::TrackCleanup CleanupPanel::readControls() const
{
    daw::TrackCleanup c;
    c.bypassed = bypass_->isChecked();
    c.dcRemoval = dc_->isChecked();
    c.highPass.enabled = highPass_->isChecked();
    c.highPass.frequencyHz = hpFrequency_->value();
    c.highPass.slopeDbPerOctave = hpSlope_->currentData().toInt();
    c.expander.enabled = expander_->isChecked();
    c.expander.thresholdDb = threshold_->value();
    c.expander.ratio = ratio_->value();
    c.expander.rangeDb = range_->value();
    c.expander.releaseMs = release_->value();
    c.stereo.enabled = stereo_->isChecked();
    c.stereo.monoBass = monoBass_->isChecked();
    c.stereo.monoBelowHz = monoBelow_->value();
    c.stereo.widthPercent = width_->value();
    c.stereo.phaseAlign = phaseAlign_->isChecked();
    c.resonance.enabled = resonance_->isChecked();
    c.resonance.harshDb = harsh_->value();
    c.resonance.shimmerDb = shimmer_->value();
    c.transient.enabled = transient_->isChecked();
    c.transient.attackDb = attack_->value();
    c.transient.sustainDb = sustain_->value();
    c.hum.enabled = hum_->isChecked();
    // Keep a detected frequency such as 50 or 60 exactly; the menu only switches between the two.
    c.hum.frequencyHz = humMains_->currentData().toDouble();
    if (const CleanupTrackInfo* info = current();
        info != nullptr && std::abs(info->cleanup.hum.frequencyHz - c.hum.frequencyHz) < 5.0)
        c.hum.frequencyHz = info->cleanup.hum.frequencyHz;
    c.hum.harmonics = humHarmonics_->value();
    c.denoise.enabled = denoise_->isChecked();
    c.denoise.reductionDb = reduction_->value();
    c.denoise.sensitivityDb = sensitivity_->value();
    if (const CleanupTrackInfo* info = current())
        c.denoise.profile = info->cleanup.denoise.profile; // learned, not edited here
    c.clamp();
    return c;
}

void CleanupPanel::edited()
{
    if (updating_)
        return;
    const CleanupTrackInfo* info = current();
    if (info == nullptr)
        return;
    const daw::TrackCleanup c = readControls();
    if (c != info->cleanup)
        Q_EMIT cleanupEdited(info->id, c);
}
