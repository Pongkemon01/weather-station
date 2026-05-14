#pragma once

#include <QMainWindow>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    enum class BannerKind { Info, Warning, Error };
    void showBanner(const QString& message, BannerKind kind);
    void hideBanner();

private:
    Ui::MainWindow* ui_;
};
