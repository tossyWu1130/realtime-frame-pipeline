#pragma once

#include <QMainWindow>
#include <QVector>
#include <QWidget>

#include <cstdint>

class QLabel;
class QPushButton;
class FramePipeline;

// Minimal custom-painted chart: no plotting dependency, so the project builds with Qt alone.
class VarianceChart : public QWidget
{
    Q_OBJECT

public:
    explicit VarianceChart(QWidget* parent = nullptr);

    void setValues(const QVector<double>& values);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<double> values_;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(bool autoRecord = false, QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void refresh();
    void toggleRecording();

private:
    FramePipeline* pipeline_ = nullptr;
    QLabel* stats_ = nullptr;
    VarianceChart* chart_ = nullptr;
    QPushButton* record_ = nullptr;

    std::uint64_t lastReceived_ = 0;
    qint64 lastSampleMs_ = 0;
};
