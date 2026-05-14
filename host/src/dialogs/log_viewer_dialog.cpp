#include "log_viewer_dialog.h"
#include "ui_log_viewer_dialog.h"

#include <QApplication>
#include <QClipboard>

LogViewerDialog::LogViewerDialog(LogBuffer* log, QWidget* parent)
    : QDialog(parent, Qt::Window)
    , ui_(new Ui::LogViewerDialog)
    , log_(log)
{
    ui_->setupUi(this);

    for (const auto& e : log_->snapshot())
        appendEntry(e);

    connect(log_, &LogBuffer::entryAdded, this, &LogViewerDialog::onEntryAdded,
            Qt::QueuedConnection);
    connect(log_, &LogBuffer::cleared, this, [this]() { ui_->logEdit->clear(); },
            Qt::QueuedConnection);
    connect(ui_->clearButton,   &QPushButton::clicked, log_,  &LogBuffer::clear);
    connect(ui_->copyAllButton, &QPushButton::clicked, this, &LogViewerDialog::onCopyAll);
}

LogViewerDialog::~LogViewerDialog()
{
    delete ui_;
}

void LogViewerDialog::onEntryAdded(const LogBuffer::Entry& entry)
{
    appendEntry(entry);
}

void LogViewerDialog::onCopyAll()
{
    QApplication::clipboard()->setText(ui_->logEdit->toPlainText());
}

void LogViewerDialog::appendEntry(const LogBuffer::Entry& entry)
{
    const char* tag = "DBG";
    switch (entry.level) {
    case LogBuffer::Level::Info:    tag = "INF"; break;
    case LogBuffer::Level::Warning: tag = "WRN"; break;
    case LogBuffer::Level::Error:   tag = "ERR"; break;
    default: break;
    }

    ui_->logEdit->appendPlainText(
        QStringLiteral("[%1] %2 %3")
        .arg(entry.when.toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(QLatin1String(tag))
        .arg(entry.message));

    if (ui_->autoScrollCheck->isChecked())
        ui_->logEdit->ensureCursorVisible();
}
