#pragma once

#include <QDialog>

#include "log_buffer.h"

QT_BEGIN_NAMESPACE
namespace Ui { class LogViewerDialog; }
QT_END_NAMESPACE

class LogViewerDialog : public QDialog {
    Q_OBJECT

public:
    explicit LogViewerDialog(LogBuffer* log, QWidget* parent = nullptr);
    ~LogViewerDialog() override;

private slots:
    void onEntryAdded(const LogBuffer::Entry& entry);
    void onCopyAll();

private:
    void appendEntry(const LogBuffer::Entry& entry);

    Ui::LogViewerDialog* ui_;
    LogBuffer*           log_;
};
