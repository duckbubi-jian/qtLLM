#pragma once

#include <QTabWidget>

class QEnterEvent;
class QEvent;
class QWidget;

namespace qtllm::ui
{
class AutoHideTabWidget final : public QTabWidget
{
    Q_OBJECT

   public:
    explicit AutoHideTabWidget(QWidget* parent = nullptr);

   protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
};
}  // namespace qtllm::ui
