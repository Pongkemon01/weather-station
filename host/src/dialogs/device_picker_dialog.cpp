#include "device_picker_dialog.h"
#include "ui_device_picker_dialog.h"

#include <QDialogButtonBox>
#include <QListWidget>

DevicePickerDialog::DevicePickerDialog(const QList<QSerialPortInfo>& devices,
                                       QWidget* parent)
    : QDialog(parent)
    , ui_(new Ui::DevicePickerDialog)
    , devices_(devices)
{
    ui_->setupUi(this);

    for (const auto& d : devices_) {
        QString label = d.portName();
        if (!d.serialNumber().isEmpty())
            label += tr(" \xe2\x80\x94 S/N: ") + d.serialNumber();
        ui_->listWidget->addItem(label);
    }

    if (!devices_.isEmpty())
        ui_->listWidget->setCurrentRow(0);

    connect(ui_->listWidget, &QListWidget::itemDoubleClicked,
            this, &QDialog::accept);
    connect(ui_->buttonBox, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(ui_->buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
}

DevicePickerDialog::~DevicePickerDialog()
{
    delete ui_;
}

QSerialPortInfo DevicePickerDialog::selectedDevice() const
{
    int row = ui_->listWidget->currentRow();
    if (row >= 0 && static_cast<qsizetype>(row) < devices_.size())
        return devices_.at(static_cast<qsizetype>(row));
    return {};
}
