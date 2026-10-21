#include "main_window.h"

#include <QApplication>
#include <QStringList>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("realtime_frame_pipeline"));

    // --record starts a 500-frame capture immediately, so the demo can be smoke-tested or
    // screenshotted without clicking anything.
    const bool autoRecord = QApplication::arguments().contains(QStringLiteral("--record"));

    MainWindow window(autoRecord);
    window.show();

    return app.exec();
}
