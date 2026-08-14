#include "MainWindow.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qtLLM"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    qtllm::ui::applyApplicationTheme(application);

    qtllm::ui::MainWindow window;
    window.show();

    return application.exec();
}
