#pragma once

#include "AgentProgress.hpp"

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace qtllm::ui
{
class AgentProgressWidget final : public QWidget
{
    Q_OBJECT

   public:
    explicit AgentProgressWidget(QWidget* parent = nullptr);

    void setSnapshot(const application::AgentProgressSnapshot& snapshot);
    [[nodiscard]] const application::AgentProgressSnapshot& snapshot() const;
    [[nodiscard]] bool isExpanded() const;

   private:
    void setExpanded(bool expanded);
    void refresh();
    void refreshPlan();
    void refreshActivities();
    void refreshElapsed();

    application::AgentProgressSnapshot snapshot_;
    QElapsedTimer elapsedClock_;
    qint64 elapsedBaseMilliseconds_ = 0;
    bool terminal_ = false;
    QTimer* elapsedTimer_ = nullptr;
    QToolButton* detailsToggle_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QWidget* details_ = nullptr;
    QWidget* plan_ = nullptr;
    QVBoxLayout* planLayout_ = nullptr;
    QLabel* operationLabel_ = nullptr;
    QLabel* waitingLabel_ = nullptr;
    QLabel* countsLabel_ = nullptr;
    QLabel* finishLabel_ = nullptr;
    QToolButton* activityToggle_ = nullptr;
    QWidget* activity_ = nullptr;
    QVBoxLayout* activityLayout_ = nullptr;
};
}  // namespace qtllm::ui
