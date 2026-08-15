#include "AutoHideTabWidget.hpp"

#include <QEvent>
#include <QTabBar>

namespace qtllm::ui
{
AutoHideTabWidget::AutoHideTabWidget(QWidget* parent) : QTabWidget(parent)
{
    tabBar()->hide();
}

void AutoHideTabWidget::enterEvent(QEnterEvent* event)
{
    tabBar()->show();
    QTabWidget::enterEvent(event);
}

void AutoHideTabWidget::leaveEvent(QEvent* event)
{
    tabBar()->hide();
    QTabWidget::leaveEvent(event);
}
}  // namespace qtllm::ui
