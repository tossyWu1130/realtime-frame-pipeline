#include "main_window.h"

#include "frame_pipeline.h"

#include <QDateTime>
#include <QDir>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kMaxHistory = 240;

QColor background() { return QColor(0x12, 0x18, 0x22); }
QColor gridColor() { return QColor(0x2a, 0x36, 0x46); }
QColor traceColor() { return QColor(0x5f, 0xd0, 0xa0); }
QColor textColor() { return QColor(0x8f, 0xa3, 0xb8); }
}  // namespace

VarianceChart::VarianceChart(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(180);
}

void VarianceChart::setValues(const QVector<double>& values)
{
    values_ = values;
    update();
}

void VarianceChart::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), background());

    const QRectF area = QRectF(rect()).adjusted(10, 10, -10, -10);

    painter.setPen(QPen(gridColor(), 1));
    for (int line = 0; line <= 4; ++line) {
        const double y = area.top() + area.height() * line / 4.0;
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }

    if (values_.size() < 2) {
        painter.setPen(textColor());
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("waiting for frames..."));
        return;
    }

    double low = values_.front();
    double high = values_.front();
    for (double value : values_) {
        low = qMin(low, value);
        high = qMax(high, value);
    }
    if (high - low < 1.0e-9) {
        high = low + 1.0;
    }

    QPainterPath path;
    for (int i = 0; i < values_.size(); ++i) {
        const double x = area.left() + area.width() * i / double(values_.size() - 1);
        const double y = area.bottom() - area.height() * (values_[i] - low) / (high - low);
        if (i == 0) {
            path.moveTo(x, y);
        } else {
            path.lineTo(x, y);
        }
    }

    painter.setPen(QPen(traceColor(), 1.5));
    painter.drawPath(path);

    // Chip behind the caption so the trace does not run through the text.
    const QString caption = QStringLiteral("variance per frame   %1 - %2")
                                .arg(low, 0, 'f', 1)
                                .arg(high, 0, 'f', 1);
    const QRectF textArea = area.adjusted(4, 4, -4, 0);
    const QFontMetricsF metrics(painter.font());
    const QRectF chip(textArea.left(), textArea.top(), metrics.horizontalAdvance(caption) + 10,
                      metrics.height() + 2);
    painter.fillRect(chip, background());

    painter.setPen(textColor());
    painter.drawText(textArea, Qt::AlignLeft | Qt::AlignTop, caption);
}

MainWindow::MainWindow(bool autoRecord, QWidget* parent)
    : QMainWindow(parent)
    , pipeline_(new FramePipeline)
{
    setWindowTitle(QStringLiteral("Frame Pipeline - 100 Hz, zero drop"));

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    stats_ = new QLabel(QStringLiteral("starting..."), central);
    stats_->setStyleSheet(QStringLiteral(
        "font-family: Consolas, 'Courier New', monospace; font-size: 12px;"
        "color: #d7e2ee; background: #121822; padding: 10px; border-radius: 4px;"));
    layout->addWidget(stats_);

    chart_ = new VarianceChart(central);
    layout->addWidget(chart_, 1);

    record_ = new QPushButton(QStringLiteral("Record 500 frames to frames.dat"), central);
    connect(record_, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    layout->addWidget(record_);

    setCentralWidget(central);
    resize(900, 480);

    pipeline_->start();

    if (autoRecord) {
        pipeline_->startRecording(QDir::current().filePath(QStringLiteral("frames.dat")).toStdString(), 500);
        record_->setText(QStringLiteral("Stop recording"));
    }

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::refresh);
    timer->start(100);

    lastSampleMs_ = QDateTime::currentMSecsSinceEpoch();
}

MainWindow::~MainWindow()
{
    pipeline_->stop();
    delete pipeline_;
}

void MainWindow::toggleRecording()
{
    const FramePipeline::Stats snapshot = pipeline_->stats();
    if (snapshot.recording) {
        pipeline_->stopRecording();
        record_->setText(QStringLiteral("Record 500 frames to frames.dat"));
        return;
    }

    const QString path = QDir::current().filePath(QStringLiteral("frames.dat"));
    pipeline_->startRecording(path.toStdString(), 500);
    record_->setText(QStringLiteral("Stop recording"));
}

void MainWindow::refresh()
{
    const FramePipeline::Stats snapshot = pipeline_->stats();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double elapsed = double(now - lastSampleMs_) / 1000.0;
    double fps = 0.0;
    if (elapsed > 0.0) {
        fps = double(snapshot.received - lastReceived_) / elapsed;
    }
    lastReceived_ = snapshot.received;
    lastSampleMs_ = now;

    stats_->setText(
        QStringLiteral("frames     gen %1    recv %2    lost %3    queue-drop %4    written %5\n"
                       "stream     fps %6    latency %7 ms    tick-late p50 %8 / p95 %9 us    rec %10")
            .arg(snapshot.generated)
            .arg(snapshot.received)
            .arg(snapshot.lost)
            .arg(snapshot.queue_drops)
            .arg(snapshot.written)
            .arg(fps, 0, 'f', 1)
            .arg(snapshot.latency_ms, 0, 'f', 2)
            .arg(snapshot.jitter_p50_us, 0, 'f', 0)
            .arg(snapshot.jitter_p95_us, 0, 'f', 0)
            .arg(snapshot.recording ? QStringLiteral("yes") : QStringLiteral("no")));

    const std::vector<double> history = pipeline_->varianceHistory();
    QVector<double> values;
    values.reserve(int(qMin<std::size_t>(history.size(), kMaxHistory)));
    for (double value : history) {
        values.push_back(value);
    }
    chart_->setValues(values);
}
