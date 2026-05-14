#pragma once

#include <QDialog>
#include <QList>
#include <QSerialPortInfo>

QT_BEGIN_NAMESPACE
namespace Ui { class DevicePickerDialog; }
QT_END_NAMESPACE

class DevicePickerDialog : public QDialog {
    Q_OBJECT

public:
    explicit DevicePickerDialog(const QList<QSerialPortInfo>& devices,
                                QWidget* parent = nullptr);
    ~DevicePickerDialog() override;

    QSerialPortInfo selectedDevice() const;

private:
    Ui::DevicePickerDialog* ui_;
    QList<QSerialPortInfo>  devices_;
};
