#pragma once

#include "AgentProgress.hpp"

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QResizeEvent;
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

   private:
    void resizeEvent(QResizeEvent* event) override;
    void refresh();
    void refreshStateText();
    void refreshPlan();
    void refreshElapsed();

    application::AgentProgressSnapshot snapshot_;
    QElapsedTimer elapsedClock_;
    qint64 elapsedBaseMilliseconds_ = 0;
    bool terminal_ = false;
    QTimer* elapsedTimer_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
    QToolButton* stateToggle_ = nullptr;
    QString stateText_;
    QString operationText_;
    QWidget* plan_ = nullptr;
    QVBoxLayout* planLayout_ = nullptr;
    QLabel* finishLabel_ = nullptr;
};
}  // namespace qtllm::ui
