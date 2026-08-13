#include "MainWindow.hpp"

#include <QLabel>

namespace qtllm::ui
{
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(tr("qtLLM"));
    resize(1100, 720);

    auto* placeholder = new QLabel(tr("Local model integration pending"), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);
}
}  // namespace qtllm::ui
