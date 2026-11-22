// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "TrackPanel.h"

#include "HelpText.h"

#include <daw/project/ProjectModel.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace {

const QColor kHeaderBackground(0x22, 0x24, 0x28);
const QColor kHeaderLine(0x33, 0x35, 0x3b);
const QColor kKnobBody(0x30, 0x32, 0x38);
const QColor kKnobTrack(0x44, 0x47, 0x4e);

// Fader taper: position p in (0, 1] maps to kTopDb - kSpanDb * (1 - p)^2,
// so the bottom of the travel is about -60 dB and position 0 is silence.
constexpr int kFaderSteps = 1000;
constexpr double kTopDb = 6.0;
constexpr double kSpanDb = 66.0;
constexpr double kDetentDb = 0.25; // snap to exactly 0 dB within this window

const char* kPanelStyle = R"(
TrackHeader QLabel { color: #c8c8c8; }
QLabel#trackNumber { color: #7d8088; }
QLabel#trackName { font-weight: bold; }
QLabel#gainLabel, QLabel#panLabel { color: #9a9da5; font: 8pt 'Consolas'; }
QToolButton#mute, QToolButton#solo {
    background: #34373d; color: #b5b8bf; border: none; border-radius: 3px;
    font-weight: bold; min-width: 22px; max-width: 22px; min-height: 20px; max-height: 20px;
}
QToolButton#mute:hover, QToolButton#solo:hover { background: #40434a; }
QToolButton#mute:checked { background: #e0a030; color: #161616; }
QToolButton#solo:checked { background: #3fbf6f; color: #161616; }
VolumeFader::groove:horizontal { height: 4px; background: #3a3d44; border-radius: 2px; }
VolumeFader::sub-page:horizontal { background: #8a8d95; border-radius: 2px; }
VolumeFader::handle:horizontal {
    width: 8px; margin: -6px 0; background: #d8d8d8; border-radius: 2px;
}
QLabel#panelTop { background: #26282d; color: #c8c8c8; padding-left: 8px; border-bottom: 1px solid #33353b; }
)";

} // namespace

// ---------------------------------------------------------------------------
// VolumeFader

VolumeFader::VolumeFader(QWidget* parent) : QSlider(Qt::Horizontal, parent)
{
    setRange(0, kFaderSteps);
    setPageStep(50);
    setSingleStep(5);
    setFocusPolicy(Qt::NoFocus);
    setValue(dbToPosition(0.0));
    setToolTip(helpTip(tr("Volume"),
                       tr("Track level, from silent (-inf) to +6 dB. 0 dB plays the stem exactly as delivered.\n"
                          "Drag the handle or scroll the wheel over it. Double-click resets to 0 dB.")));
    connect(this, &QSlider::valueChanged, this, [this](int position) {
        if (updating_)
            return;
        double db = positionToDb(position);
        if (std::abs(db) < kDetentDb)
            db = 0.0;
        gainDb_ = db;
        Q_EMIT gainDbChanged(db);
    });
}

double VolumeFader::positionToDb(int position)
{
    if (position <= 0)
        return daw::kSilenceDb;
    const double fromTop = 1.0 - static_cast<double>(position) / kFaderSteps;
    return kTopDb - kSpanDb * fromTop * fromTop;
}

int VolumeFader::dbToPosition(double db)
{
    if (db <= daw::kSilenceDb)
        return 0;
    const double fromTop = std::sqrt(std::max(0.0, (kTopDb - db) / kSpanDb));
    return std::clamp(static_cast<int>(std::lround((1.0 - fromTop) * kFaderSteps)), 1, kFaderSteps);
}

QString VolumeFader::formatDb(double db)
{
    if (db <= daw::kSilenceDb)
        return QStringLiteral("-inf");
    return QStringLiteral("%1%2 dB").arg(db > 0.05 ? QStringLiteral("+") : QString()).arg(db, 0, 'f', 1);
}

void VolumeFader::setGainDb(double db)
{
    gainDb_ = db;
    updating_ = true;
    setValue(dbToPosition(db));
    updating_ = false;
}

void VolumeFader::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return QSlider::mouseDoubleClickEvent(event);
    setGainDb(0.0);
    Q_EMIT gainDbChanged(0.0);
}

// ---------------------------------------------------------------------------
// PanKnob

PanKnob::PanKnob(QWidget* parent) : QWidget(parent)
{
    setFixedSize(sizeHint());
    setCursor(Qt::SizeVerCursor);
    setToolTip(helpTip(tr("Pan"),
                       tr("Moves the track between the left and right speakers. Centre (C) leaves the stem untouched.\n"
                          "Drag up or right to pan right, down or left to pan left; hold Shift for fine steps.\n"
                          "Scroll the wheel over it to nudge. Double-click returns to centre.")));
}

QString PanKnob::formatPan(double pan)
{
    const int percent = static_cast<int>(std::lround(pan * 100.0));
    if (percent == 0)
        return QStringLiteral("C");
    return percent < 0 ? QStringLiteral("L%1").arg(-percent) : QStringLiteral("R%1").arg(percent);
}

void PanKnob::setPan(double pan)
{
    pan = std::clamp(pan, -1.0, 1.0);
    if (pan == pan_)
        return;
    pan_ = pan;
    update();
}

void PanKnob::setAccent(const QColor& color)
{
    accent_ = color;
    update();
}

void PanKnob::setPanFromUser(double pan)
{
    pan = std::clamp(std::round(pan * 100.0) / 100.0, -1.0, 1.0);
    if (pan == pan_)
        return;
    pan_ = pan;
    update();
    Q_EMIT panChanged(pan_);
}

void PanKnob::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF ring = QRectF(rect()).adjusted(3, 3, -3, -3);

    p.setPen(Qt::NoPen);
    p.setBrush(kKnobBody);
    p.drawEllipse(ring.adjusted(3, 3, -3, -3));

    // 270 degree travel from 225 deg (lower left) clockwise to -45 deg (lower right).
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kKnobTrack, 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(ring, 225 * 16, -270 * 16);
    if (pan_ != 0.0) {
        p.setPen(QPen(accent_, 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(ring, 90 * 16, static_cast<int>(-pan_ * 135.0 * 16.0));
    }

    const double angle = (90.0 - pan_ * 135.0) * std::numbers::pi / 180.0;
    const QPointF centre = ring.center();
    const double radius = ring.width() / 2.0 - 3.0;
    p.setPen(QPen(QColor(0xe6, 0xe6, 0xe6), 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(centre, centre + QPointF(std::cos(angle) * radius, -std::sin(angle) * radius));
}

void PanKnob::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    dragOrigin_ = event->position();
    dragStartPan_ = pan_;
}

void PanKnob::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton))
        return;
    const QPointF delta = event->position() - dragOrigin_;
    const double pixelsPerFullRange = (event->modifiers() & Qt::ShiftModifier) ? 600.0 : 150.0;
    double pan = dragStartPan_ + (delta.x() - delta.y()) * 2.0 / pixelsPerFullRange;
    if (std::abs(pan) < 0.02)
        pan = 0.0; // centre detent
    setPanFromUser(pan);
}

void PanKnob::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        setPanFromUser(0.0);
}

void PanKnob::wheelEvent(QWheelEvent* event)
{
    const double notches = event->angleDelta().y() / 120.0;
    const double step = (event->modifiers() & Qt::ShiftModifier) ? 0.01 : 0.05;
    setPanFromUser(pan_ + notches * step);
    event->accept();
}

// ---------------------------------------------------------------------------
// TrackHeader

TrackHeader::TrackHeader(QWidget* parent) : QWidget(parent)
{
    number_ = new QLabel(this);
    number_->setObjectName(QStringLiteral("trackNumber"));
    number_->setFixedWidth(18);
    name_ = new QLabel(this);
    name_->setObjectName(QStringLiteral("trackName"));
    name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    mute_ = new QToolButton(this);
    mute_->setObjectName(QStringLiteral("mute"));
    mute_->setText(QStringLiteral("M"));
    mute_->setCheckable(true);
    mute_->setFocusPolicy(Qt::NoFocus);
    mute_->setToolTip(helpTip(tr("Mute"),
                              tr("Silences this track in playback and in the exported mix.\n"
                                 "A muted track stays silent even when it is soloed.")));

    solo_ = new QToolButton(this);
    solo_->setObjectName(QStringLiteral("solo"));
    solo_->setText(QStringLiteral("S"));
    solo_->setCheckable(true);
    solo_->setFocusPolicy(Qt::NoFocus);
    solo_->setToolTip(helpTip(tr("Solo"),
                              tr("Plays only the soloed tracks so you can hear them on their own.\n"
                                 "Solo several tracks to hear them together. Click again to un-solo.\n"
                                 "Solo affects the exported mix too, so clear it before exporting.")));

    fader_ = new VolumeFader(this);
    gainLabel_ = new QLabel(VolumeFader::formatDb(0.0), this);
    gainLabel_->setObjectName(QStringLiteral("gainLabel"));
    gainLabel_->setFixedWidth(50);
    gainLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gainLabel_->setToolTip(helpTip(tr("Track volume"),
                                   tr("Current fader level in decibels. -6 dB is about half as loud; -inf is silent.")));

    pan_ = new PanKnob(this);
    panLabel_ = new QLabel(PanKnob::formatPan(0.0), this);
    panLabel_->setObjectName(QStringLiteral("panLabel"));
    panLabel_->setFixedWidth(30);
    panLabel_->setToolTip(helpTip(tr("Pan position"),
                                  tr("C = centre. L50 = halfway left, R100 = fully right.\n"
                                     "Panning one way turns the opposite speaker down; the near side is never boosted.")));
    number_->setToolTip(helpTip(tr("Track number"), tr("Position of the track in the arrangement.")));

    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    top->addWidget(number_);
    top->addWidget(name_, 1);
    top->addWidget(mute_);
    top->addWidget(solo_);

    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(4);
    bottom->addWidget(fader_, 1);
    bottom->addWidget(gainLabel_);
    bottom->addSpacing(4);
    bottom->addWidget(pan_);
    bottom->addWidget(panLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 6, 8, 6);
    layout->setSpacing(4);
    layout->addLayout(top);
    layout->addLayout(bottom);
    layout->addStretch(1);

    connect(mute_, &QToolButton::toggled, this, [this](bool on) {
        state_.muted = on;
        Q_EMIT muteToggled(on);
    });
    connect(solo_, &QToolButton::toggled, this, [this](bool on) {
        state_.soloed = on;
        Q_EMIT soloToggled(on);
    });
    connect(fader_, &VolumeFader::gainDbChanged, this, [this](double db) {
        state_.gainDb = db;
        gainLabel_->setText(VolumeFader::formatDb(db));
        Q_EMIT gainChanged(db);
    });
    connect(pan_, &PanKnob::panChanged, this, [this](double pan) {
        state_.pan = pan;
        panLabel_->setText(PanKnob::formatPan(pan));
        Q_EMIT panChanged(pan);
    });
}

void TrackHeader::setState(const TrackHeaderState& state)
{
    const bool nameChanged = state.name != state_.name || state.number != state_.number;
    const bool lookChanged = state.color != state_.color || state.audible != state_.audible;
    state_ = state;

    if (mute_->isChecked() != state.muted) {
        const QSignalBlocker block(mute_);
        mute_->setChecked(state.muted);
    }
    if (solo_->isChecked() != state.soloed) {
        const QSignalBlocker block(solo_);
        solo_->setChecked(state.soloed);
    }
    if (fader_->gainDb() != state.gainDb && !fader_->isSliderDown())
        fader_->setGainDb(state.gainDb);
    gainLabel_->setText(VolumeFader::formatDb(state.gainDb));
    pan_->setPan(state.pan);
    pan_->setAccent(state.color);
    panLabel_->setText(PanKnob::formatPan(state.pan));

    if (nameChanged) {
        number_->setText(QString::number(state.number));
        name_->setToolTip(helpTip(state.name, tr("Track %1. The colour strip is picked from the stem name "
                                                  "(vocals, drums, bass, guitar, keys, synth...).")
                                                   .arg(state.number)));
        updateNameText();
    }
    if (lookChanged) {
        name_->setStyleSheet(state.audible ? QString() : QStringLiteral("color: #6c6f76;"));
        update();
    }
}

void TrackHeader::updateNameText()
{
    name_->setText(name_->fontMetrics().elidedText(state_.name, Qt::ElideRight, std::max(10, name_->width())));
}

void TrackHeader::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateNameText();
}

void TrackHeader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kHeaderBackground);
    QColor strip = state_.color.isValid() ? state_.color : QColor(0x6b, 0x7d, 0x99);
    if (!state_.audible)
        strip = strip.darker(220);
    p.fillRect(QRect(0, 0, 4, height() - 1), strip);
    p.setPen(kHeaderLine);
    p.drawLine(0, height() - 1, width(), height() - 1);
}

// ---------------------------------------------------------------------------
// TrackPanel

TrackPanel::TrackPanel(int rulerHeight, QWidget* parent) : QWidget(parent)
{
    setStyleSheet(QString::fromLatin1(kPanelStyle));
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x17, 0x18, 0x1b));
    setPalette(pal);

    top_ = new QLabel(this);
    top_->setObjectName(QStringLiteral("panelTop"));
    top_->setFixedHeight(rulerHeight);
    top_->setToolTip(helpTip(tr("Tempo and time signature"),
                             tr("Used for the bar.beat position display. Automatic tempo detection is planned.")));
    lanes_ = new QWidget(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(top_);
    layout->addWidget(lanes_, 1);
}

void TrackPanel::setTopText(const QString& text)
{
    top_->setText(text);
}

void TrackPanel::setTracks(const std::vector<TrackHeaderState>& tracks)
{
    std::unordered_map<QString, TrackHeader*> existing;
    for (TrackHeader* header : headers_)
        existing.emplace(header->trackId(), header);

    std::vector<TrackHeader*> ordered;
    ordered.reserve(tracks.size());
    for (const TrackHeaderState& state : tracks) {
        TrackHeader* header = nullptr;
        if (auto it = existing.find(state.id); it != existing.end()) {
            header = it->second;
            existing.erase(it);
        } else {
            header = new TrackHeader(lanes_);
            const QString id = state.id;
            connect(header, &TrackHeader::gainChanged, this, [this, id](double db) { Q_EMIT gainChanged(id, db); });
            connect(header, &TrackHeader::panChanged, this, [this, id](double pan) { Q_EMIT panChanged(id, pan); });
            connect(header, &TrackHeader::muteToggled, this, [this, id](bool on) { Q_EMIT muteToggled(id, on); });
            connect(header, &TrackHeader::soloToggled, this, [this, id](bool on) { Q_EMIT soloToggled(id, on); });
            header->show();
        }
        header->setState(state);
        ordered.push_back(header);
    }
    for (auto& [id, header] : existing)
        header->deleteLater();
    headers_ = std::move(ordered);
    layoutHeaders();
}

void TrackPanel::setLaneLayout(int laneHeight, int scrollY)
{
    if (laneHeight == laneHeight_ && scrollY == scrollY_)
        return;
    laneHeight_ = laneHeight;
    scrollY_ = scrollY;
    layoutHeaders();
}

void TrackPanel::layoutHeaders()
{
    for (std::size_t i = 0; i < headers_.size(); ++i)
        headers_[i]->setGeometry(0, static_cast<int>(i) * laneHeight_ - scrollY_, width(), laneHeight_);
}

void TrackPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutHeaders();
}

void TrackPanel::wheelEvent(QWheelEvent* event)
{
    Q_EMIT scrollRequested(event->angleDelta().y());
    event->accept();
}
