// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "StemReportPanel.h"

#include "HelpText.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

enum Column { ColTrack, ColShare, ColLoudness, ColPeak, ColActive, ColFloor, ColLowEnd, ColStereo, ColCleanup, ColNotes, ColCount };

constexpr int kShareRole = Qt::UserRole + 1;
constexpr int kTrackRole = Qt::UserRole + 2;
constexpr int kAdviceRole = Qt::UserRole + 3;

QColor severityColour(daw::AdviceSeverity severity)
{
    switch (severity) {
    case daw::AdviceSeverity::Warning: return QColor(0xe5, 0x53, 0x3d);
    case daw::AdviceSeverity::Suggestion: return QColor(0xe0, 0xa0, 0x30);
    case daw::AdviceSeverity::Info: return QColor(0x4a, 0x90, 0xd9);
    }
    return Qt::gray;
}

QIcon severityIcon(daw::AdviceSeverity severity)
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(severityColour(severity));
    p.drawEllipse(QRectF(3, 3, 26, 26));
    p.setPen(QColor(0x1b, 0x1d, 0x21));
    QFont font(QStringLiteral("Segoe UI"));
    font.setBold(true);
    font.setPixelSize(20);
    p.setFont(font);
    const QString glyph = severity == daw::AdviceSeverity::Warning      ? QStringLiteral("!")
                          : severity == daw::AdviceSeverity::Suggestion ? QStringLiteral("→")
                                                                         : QStringLiteral("i");
    p.drawText(QRectF(0, 0, 32, 32), Qt::AlignCenter, glyph);
    return QIcon(pixmap);
}

QIcon swatch(const QColor& colour)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(colour);
    return QIcon(pixmap);
}

QString db(double value, const char* unit = "")
{
    if (value <= daw::kLoudnessFloor + 1.0)
        return QStringLiteral("-inf");
    return QString::number(value, 'f', 1) + QString::fromLatin1(unit);
}

QString percent(double fraction)
{
    return QString::number(fraction * 100.0, 'f', fraction < 0.1 ? 1 : 0) + QStringLiteral("%");
}

/// Draws the level share as a bar behind the number.
class ShareDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    double maxShare = 1.0;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const double share = index.data(kShareRole).toDouble();
        if (share > 0.0 && maxShare > 0.0) {
            QRectF bar = QRectF(option.rect).adjusted(2, 4, -2, -4);
            bar.setWidth(bar.width() * std::clamp(share / maxShare, 0.0, 1.0));
            painter->fillRect(bar, QColor(0x3d, 0x6f, 0xa8, 150));
        }
        QStyledItemDelegate::paint(painter, option, index);
    }
};

} // namespace

StemReportPanel::StemReportPanel(QWidget* parent) : QWidget(parent)
{
    mix_ = new QLabel(this);
    mix_->setTextFormat(Qt::RichText);
    mix_->setStyleSheet(QStringLiteral("font-size: 10pt;"));
    status_ = new QLabel(this);
    status_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
    analyze_ = new QPushButton(tr("Analyze"), this);
    analyze_->setToolTip(helpTip(tr("Analyze stems"), tr("Measures every stem and the mix again and updates the advice.\n"
                                                          "Runs on its own after importing; run it again after changing faders.\n"
                                                          "Analysis never changes the audio.")));
    connect(analyze_, &QPushButton::clicked, this, &StemReportPanel::analyzeRequested);

    auto* header = new QHBoxLayout;
    header->addWidget(mix_, 1);
    header->addWidget(status_);
    header->addWidget(analyze_);

    table_ = new QTableWidget(0, ColCount, this);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setItemDelegateForColumn(ColShare, new ShareDelegate(table_));
    const std::pair<QString, QString> columns[ColCount] = {
        {tr("Stem"), tr("Track name, in arrangement order.")},
        {tr("Share"), tr("How much of the total energy of all stems this stem contributes, after its fader and pan.")},
        {tr("Loudness"), tr("Integrated loudness of the stem on its own (LUFS), after its fader.")},
        {tr("Peak"), tr("Highest sample level of the stem (dBFS), after its fader.")},
        {tr("Active"), tr("How much of the song the stem actually plays (above its noise bed).")},
        {tr("Noise bed"), tr("Level of the hiss, hum or separation noise between the notes (dBFS RMS).\n"
                             "Empty when the stem never rests or rests in true silence.\n"
                             "Above about -60 dBFS it is audible in quiet passages.")},
        {tr("Low end"), tr("Share of the stem's energy below 120 Hz. Kick and bass belong here;\n"
                           "low end in other stems muddies the mix.")},
        {tr("Stereo"), tr("Correlation of left and right: 1 = mono, 0 = unrelated (very wide), below 0 = out of phase.")},
        {tr("Clean-up"), tr("The stem's clean-up chain when it was measured. The numbers in this row are measured after it.")},
        {tr("Notes"), tr("How many advice items concern this stem.")},
    };
    for (int c = 0; c < ColCount; ++c) {
        auto* item = new QTableWidgetItem(columns[c].first);
        item->setToolTip(helpTip(columns[c].first, columns[c].second));
        table_->setHorizontalHeaderItem(c, item);
    }
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ColTrack, QHeaderView::Stretch);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    advice_ = new QListWidget(this);
    advice_->setWordWrap(true);
    advice_->setIconSize(QSize(16, 16));
    advice_->setResizeMode(QListView::Adjust);
    advice_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    advice_->setSpacing(2);
    advice_->setToolTip(helpTip(tr("Advice"), tr("What the measurements suggest, most important first.\n"
                                                 "Red: a problem to fix. Orange: a suggested clean-up. Blue: information.\n"
                                                 "Click an item to select its stem. Items with a \"Fix\" line can set up the clean-up for you.")));
    connect(advice_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const int track = item->data(kTrackRole).toInt();
        if (track < 0)
            return;
        for (int row = 0; row < table_->rowCount(); ++row)
            if (table_->item(row, ColTrack)->data(kTrackRole).toInt() == track)
                table_->selectRow(row);
        Q_EMIT trackActivated(track);
    });
    connect(advice_, &QListWidget::currentRowChanged, this, &StemReportPanel::updateFixButtons);

    applyFix_ = new QPushButton(tr("Apply Fix"), this);
    applyFix_->setToolTip(helpTip(tr("Apply fix"), tr("Sets up the clean-up the selected advice describes (high-pass, expander or DC removal).\n"
                                                       "Nothing is rendered: the change is live and can be edited or bypassed in the Clean-up panel.")));
    applyAll_ = new QPushButton(tr("Apply All Fixes"), this);
    applyAll_->setToolTip(helpTip(tr("Apply all fixes"), tr("Sets up every clean-up the report suggests, on all stems at once.\n"
                                                             "Use Bypass All Clean-up (Ctrl+B) to compare with the original.")));
    connect(applyFix_, &QPushButton::clicked, this, [this] {
        const int row = advice_->currentRow();
        const QListWidgetItem* item = advice_->currentItem();
        const int index = item != nullptr ? item->data(kAdviceRole).toInt() : -1;
        if (row >= 0 && index >= 0 && static_cast<std::size_t>(index) < adviceFixes_.size())
            Q_EMIT applyFixesRequested(adviceFixes_[static_cast<std::size_t>(index)]);
    });
    connect(applyAll_, &QPushButton::clicked, this, [this] {
        std::vector<daw::CleanupFix> all;
        for (const auto& fixes : adviceFixes_)
            all.insert(all.end(), fixes.begin(), fixes.end());
        if (!all.empty())
            Q_EMIT applyFixesRequested(all);
    });
    auto* adviceButtons = new QHBoxLayout;
    adviceButtons->setContentsMargins(0, 0, 0, 0);
    adviceButtons->addStretch(1);
    adviceButtons->addWidget(applyFix_);
    adviceButtons->addWidget(applyAll_);
    auto* adviceBox = new QWidget(this);
    auto* adviceLayout = new QVBoxLayout(adviceBox);
    adviceLayout->setContentsMargins(0, 0, 0, 0);
    adviceLayout->setSpacing(4);
    adviceLayout->addWidget(advice_, 1);
    adviceLayout->addLayout(adviceButtons);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(table_);
    splitter->addWidget(adviceBox);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->addLayout(header);
    layout->addWidget(splitter, 1);

    setReport(nullptr, {}, false, false);
}

void StemReportPanel::setReport(const daw::StemReport* report, const std::vector<QColor>& colors, bool stale, bool running)
{
    running_ = running;
    if (report == nullptr) {
        mix_->setText(tr("<b>Stem report</b> <span style='color:#9a9da5'>Import stems: they are analysed automatically.</span>"));
        table_->setRowCount(0);
        advice_->clear();
        adviceFixes_.clear();
    } else {
        const auto& m = report->mix;
        const bool clips = m.truePeakDbtp > 0.0 || report->mixOvers > 0;
        const bool overCeiling = m.truePeakDbtp > -1.0;
        const QString peakColour = clips ? QStringLiteral("#ff5a4a") : overCeiling ? QStringLiteral("#e0a030") : QStringLiteral("#e6e6e6");
        const double seconds = static_cast<double>(report->frames) / report->sampleRate;
        mix_->setText(tr("<b>Mix</b>&nbsp; %1 LUFS integrated &nbsp;&middot;&nbsp; range %2 LU &nbsp;&middot;&nbsp; "
                         "true peak <span style='color:%3'>%4 dBTP%5</span> &nbsp;&middot;&nbsp; "
                         "<span style='color:#9a9da5'>%6:%7, %n stem(s)</span>",
                         nullptr, static_cast<int>(report->tracks.size()))
                          .arg(db(m.integratedLufs))
                          .arg(m.loudnessRangeLu, 0, 'f', 1)
                          .arg(peakColour)
                          .arg((m.truePeakDbtp > 0.0 ? QStringLiteral("+") : QString()) + db(m.truePeakDbtp))
                          .arg(clips ? tr(" (clips)") : QString())
                          .arg(static_cast<int>(seconds) / 60)
                          .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0')));
        fillTable(*report, colors);
        fillAdvice(*report);
    }
    updateStatus(report != nullptr, stale, running);
    updateFixButtons();
}

void StemReportPanel::updateFixButtons()
{
    std::size_t total = 0;
    for (const auto& fixes : adviceFixes_)
        total += fixes.size();
    const QListWidgetItem* item = advice_->currentItem();
    const int index = item != nullptr ? item->data(kAdviceRole).toInt() : -1;
    const bool selectedHasFix = index >= 0 && static_cast<std::size_t>(index) < adviceFixes_.size()
                                && !adviceFixes_[static_cast<std::size_t>(index)].empty();
    applyFix_->setEnabled(!running_ && selectedHasFix);
    applyAll_->setEnabled(!running_ && total > 0);
    applyAll_->setText(total > 0 ? tr("Apply All Fixes (%1)").arg(total) : tr("Apply All Fixes"));
}

QString StemReportPanel::describeFix(const daw::StemReport& report, const daw::CleanupFix& fix) const
{
    QStringList parts;
    if (fix.highPass && fix.highPass->enabled)
        parts << tr("high-pass at %1 Hz, %2 dB/octave").arg(fix.highPass->frequencyHz, 0, 'f', 0).arg(fix.highPass->slopeDbPerOctave);
    if (fix.expander && fix.expander->enabled)
        parts << tr("expander at %1 dB, ratio 1:%2, range %3 dB")
                     .arg(fix.expander->thresholdDb, 0, 'f', 0)
                     .arg(fix.expander->ratio, 0, 'g', 3)
                     .arg(fix.expander->rangeDb, 0, 'f', 0);
    if (fix.dcRemoval && *fix.dcRemoval)
        parts << tr("DC removal");
    const QString name = fix.track < report.tracks.size() ? QString::fromStdString(report.tracks[fix.track].name) : QString();
    return name + QStringLiteral(": ") + parts.join(QStringLiteral(", "));
}

void StemReportPanel::setProgress(double fraction)
{
    if (running_)
        status_->setText(tr("Analysing... %1%").arg(static_cast<int>(fraction * 100.0)));
}

void StemReportPanel::updateStatus(bool hasReport, bool stale, bool running)
{
    analyze_->setEnabled(!running);
    if (running) {
        status_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
        status_->setText(tr("Analysing..."));
    } else if (hasReport && stale) {
        status_->setStyleSheet(QStringLiteral("color: #e0a030;"));
        status_->setText(tr("Out of date: the mix changed"));
    } else {
        status_->setStyleSheet(QStringLiteral("color: #9a9da5;"));
        status_->setText(hasReport ? tr("Up to date") : QString());
    }
}

void StemReportPanel::fillTable(const daw::StemReport& report, const std::vector<QColor>& colors)
{
    std::vector<int> notes(report.tracks.size(), 0);
    for (const auto& a : report.advice) {
        if (a.track && *a.track < notes.size())
            ++notes[*a.track];
    }
    double maxShare = 0.0;
    for (const auto& t : report.tracks)
        maxShare = std::max(maxShare, t.levelShare);
    static_cast<ShareDelegate*>(table_->itemDelegateForColumn(ColShare))->maxShare = maxShare;

    table_->setRowCount(static_cast<int>(report.tracks.size()));
    for (std::size_t i = 0; i < report.tracks.size(); ++i) {
        const auto& t = report.tracks[i];
        const int row = static_cast<int>(i);
        const bool dim = t.muted || t.silent;
        auto set = [&](int column, const QString& text, const QColor& colour = QColor()) {
            auto* item = new QTableWidgetItem(text);
            if (column != ColTrack)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (dim)
                item->setForeground(QColor(0x70, 0x73, 0x7a));
            else if (colour.isValid())
                item->setForeground(colour);
            table_->setItem(row, column, item);
            return item;
        };
        QString name = QString::fromStdString(t.name);
        if (t.muted)
            name += tr("  (muted)");
        auto* nameItem = set(ColTrack, name);
        nameItem->setIcon(swatch(i < colors.size() ? colors[i] : QColor(Qt::gray)));
        nameItem->setData(kTrackRole, row);
        set(ColShare, percent(t.levelShare))->setData(kShareRole, t.levelShare);
        if (t.silent) {
            for (int c = ColLoudness; c < ColNotes; ++c)
                set(c, QStringLiteral("-"));
        } else {
            set(ColLoudness, db(t.loudness.integratedLufs));
            set(ColPeak, db(t.loudness.samplePeakDbfs),
                t.loudness.samplePeakDbfs > -1.0 ? QColor(0xff, 0x5a, 0x4a) : QColor());
            set(ColActive, percent(t.activeFraction));
            set(ColFloor, t.noiseFloorDbfs ? db(*t.noiseFloorDbfs) : QStringLiteral("-"),
                t.noiseFloorDbfs && *t.noiseFloorDbfs > -60.0 ? QColor(0xe0, 0xa0, 0x30) : QColor());
            set(ColLowEnd, percent(t.lowEndShare));
            QString stereo = QStringLiteral("-");
            QColor stereoColour;
            if (t.stereoCorrelation) {
                stereo = *t.stereoCorrelation > 0.999 ? tr("mono") : QString::number(*t.stereoCorrelation, 'f', 2);
                if (*t.stereoCorrelation < -0.2)
                    stereoColour = QColor(0xe5, 0x53, 0x3d);
            }
            set(ColStereo, stereo, stereoColour);
        }
        const QString cleanup = QString::fromStdString(daw::describeCleanup(t.cleanup));
        auto* cleanupItem = set(ColCleanup, cleanup.isEmpty() ? QStringLiteral("-") : cleanup,
                                t.cleanup.active() ? QColor(0x6f, 0xb3, 0xf0) : QColor());
        cleanupItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        set(ColNotes, notes[i] > 0 ? QString::number(notes[i]) : QString());
    }
}

void StemReportPanel::fillAdvice(const daw::StemReport& report)
{
    advice_->clear();
    adviceFixes_.clear();
    for (std::size_t i = 0; i < report.advice.size(); ++i) {
        const auto& a = report.advice[i];
        QString text = QString::fromStdString(a.message);
        for (const auto& fix : a.fixes)
            text += QStringLiteral("\n") + tr("Fix: %1").arg(describeFix(report, fix));
        auto* item = new QListWidgetItem(severityIcon(a.severity), text, advice_);
        item->setData(kTrackRole, a.track ? static_cast<int>(*a.track) : -1);
        item->setData(kAdviceRole, static_cast<int>(i));
        item->setToolTip(text);
        adviceFixes_.push_back(a.fixes);
    }
    if (report.advice.empty())
        new QListWidgetItem(tr("Nothing to report: the stems look clean."), advice_);
}
