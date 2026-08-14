#pragma once

#include "ToolPolicy.hpp"

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPushButton;

namespace qtllm::ui
{
class ToolApprovalWidget final : public QWidget
{
    Q_OBJECT

   public:
    explicit ToolApprovalWidget(infrastructure::mcp::ToolRisk risk,
                                const QString& toolName,
                                const QJsonObject& arguments,
                                QWidget* parent = nullptr);

    void markCancelled();

   signals:
    void decisionMade(bool approved);

   private:
    void resolve(bool approved);
    void setResolvedState(const QString& status, bool approved);

    QLabel* statusLabel_ = nullptr;
    QPushButton* allowButton_ = nullptr;
    QPushButton* rejectButton_ = nullptr;
    bool resolved_ = false;
};
}  // namespace qtllm::ui
