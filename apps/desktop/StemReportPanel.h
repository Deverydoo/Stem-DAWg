// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/session/StemReport.h>

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;

/// Shows the stem report: the mix summary, one row of measurements per stem
/// and the advice list. Clicking advice about a stem selects its row; advice
/// with a clean-up fix can be applied from here.
class StemReportPanel final : public QWidget {
    Q_OBJECT

public:
    explicit StemReportPanel(QWidget* parent = nullptr);

    /// `report` may be null (no report yet). `trackColors` follows the report's track order.
    void setReport(const daw::StemReport* report, const std::vector<QColor>& trackColors, bool stale, bool running);
    void setProgress(double fraction);

Q_SIGNALS:
    void analyzeRequested();
    void trackActivated(int trackIndex);
    void applyFixesRequested(const std::vector<daw::CleanupFix>& fixes);

private:
    void fillTable(const daw::StemReport& report, const std::vector<QColor>& colors);
    void fillAdvice(const daw::StemReport& report);
    void updateStatus(bool hasReport, bool stale, bool running);
    void updateFixButtons();
    QString describeFix(const daw::StemReport& report, const daw::CleanupFix& fix) const;

    QLabel* mix_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* analyze_ = nullptr;
    QTableWidget* table_ = nullptr;
    QListWidget* advice_ = nullptr;
    QPushButton* applyFix_ = nullptr;
    QPushButton* applyAll_ = nullptr;
    std::vector<std::vector<daw::CleanupFix>> adviceFixes_; // per advice item
    bool running_ = false;
};
