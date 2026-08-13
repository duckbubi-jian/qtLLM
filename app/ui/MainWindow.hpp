#pragma once

#include <QMainWindow>

namespace qtllm::ui
{
class MainWindow final : public QMainWindow
{
    Q_OBJECT

   public:
    explicit MainWindow(QWidget* parent = nullptr);
};
}  // namespace qtllm::ui
