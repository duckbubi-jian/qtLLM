#include "MainWindow.hpp"
#include "Theme.hpp"

#include "Logging.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QTextStream>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qtLLM"));
    QGuiApplication::setApplicationDisplayName(
        QStringLiteral("qtLLM - Local AI Assistant (仅学习可用)"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    application.setWindowIcon(QIcon(QStringLiteral(":/qtllm/qtllm-icon.png")));
    QString loggingError;
    if (!qtllm::logging::initialize(QStringLiteral("app"), &loggingError))
        QTextStream(stderr) << "warning: " << loggingError << Qt::endl;
    qtllm::logging::installQtMessageHandler();
    qtllm::logging::info(QStringLiteral("qtLLM 0.2.0 starting; log=%1")
                             .arg(qtllm::logging::logFilePath()));
    qtllm::ui::applyApplicationTheme(application);

    qtllm::ui::MainWindow window;
    window.show();

    const auto result = application.exec();
    qtllm::logging::info(
        QStringLiteral("qtLLM exiting with code %1").arg(result));
    qtllm::logging::shutdown();
    return result;
}
