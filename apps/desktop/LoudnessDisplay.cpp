// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "LoudnessDisplay.h"

#include "HelpText.h"

#include <QMouseEvent>

namespace {

QString value(double db, int width = 5)
{
    if (db <= daw::kLoudnessFloor + 1.0)
        return QStringLiteral("--").rightJustified(width);
    return QString::number(db, 'f', 1).rightJustified(width);
}

} // namespace

LoudnessDisplay::LoudnessDisplay(QWidget* parent) : QLabel(parent)
{
    setTextFormat(Qt::RichText);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("QLabel { font: 9pt 'Consolas'; padding: 0 10px; }"));
    setToolTip(helpTip(tr("Loudness (EBU R 128)"),
                       tr("Measured live while the song plays, as streaming services and broadcasters measure it.\n"
                          "M: momentary loudness (last 0.4 s).\n"
                          "S: short-term loudness (last 3 s).\n"
                          "I: integrated loudness of everything played since the last jump or reset. "
                          "Play the whole song from the start to get the song's loudness.\n"
                          "LRA: loudness range, how much the loudness varies (LU).\n"
                          "TP: highest true peak, including peaks between samples. Keep it at or below -1 dBTP.\n"
                          "Streaming services play songs at about -14 LUFS. Click to restart the measurement.")));
    setReading({}, false);
}

void LoudnessDisplay::setReading(const daw::LoudnessReading& r, bool playing)
{
    const QString label = playing ? QStringLiteral("#8a8d95") : QStringLiteral("#5d6067");
    const QString number = playing ? QStringLiteral("#e6e6e6") : QStringLiteral("#9a9da5");
    const bool overCeiling = r.truePeakDbtp > -1.0;
    const QString peak = overCeiling ? QStringLiteral("#ff5a4a") : number;
    const auto item = [&](const QString& name, const QString& text, const QString& colour) {
        return QStringLiteral("<span style='color:%1'>%2</span>&nbsp;<span style='color:%3'>%4</span>")
            .arg(label, name, colour, text.toHtmlEscaped().replace(QLatin1Char(' '), QStringLiteral("&nbsp;")));
    };
    const QString lra = r.loudnessRangeLu > 0.0 ? QString::number(r.loudnessRangeLu, 'f', 1).rightJustified(4) : QStringLiteral("  --");
    const QString html = item(QStringLiteral("M"), value(r.momentaryLufs), number) + QStringLiteral("&nbsp;&nbsp;")
                         + item(QStringLiteral("S"), value(r.shortTermLufs), number) + QStringLiteral("&nbsp;&nbsp;")
                         + item(QStringLiteral("I"), value(r.integratedLufs), number)
                         + QStringLiteral("&nbsp;<span style='color:%1'>LUFS</span>&nbsp;&nbsp;&nbsp;").arg(label)
                         + item(QStringLiteral("LRA"), lra, number) + QStringLiteral("&nbsp;&nbsp;&nbsp;")
                         + item(QStringLiteral("TP"), value(r.truePeakDbtp), peak);
    if (html != shown_) {
        shown_ = html;
        setText(html);
    }
}

void LoudnessDisplay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        Q_EMIT resetRequested();
    QLabel::mousePressEvent(event);
}
