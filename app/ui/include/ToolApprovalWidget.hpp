#pragma once

#include "ToolPolicy.hpp"

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace qtllm::ui
{
enum class ToolApprovalDecision
{
    DenyOnce,
    AllowOnce,
    AlwaysAllow
};

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
    void decisionMade(qtllm::ui::ToolApprovalDecision decision);

   private:
    void resolve(ToolApprovalDecision decision);
    void setResolvedState(const QString& status, bool approved);

    QLabel* statusLabel_ = nullptr;
    QWidget* details_ = nullptr;
    QToolButton* detailsToggle_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QPlainTextEdit* argumentView_ = nullptr;
    QToolButton* argumentToggle_ = nullptr;
    QPushButton* allowButton_ = nullptr;
    QPushButton* alwaysAllowButton_ = nullptr;
    QPushButton* rejectButton_ = nullptr;
    bool resolved_ = false;
};
}  // namespace qtllm::ui

Q_DECLARE_METATYPE(qtllm::ui::ToolApprovalDecision)
