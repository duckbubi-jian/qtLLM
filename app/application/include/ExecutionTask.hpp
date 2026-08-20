#pragma once

#include "AgentTask.hpp"
#include "ToolResult.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <optional>

namespace qtllm::application
{
class ExecutionTask final : public AgentTask
{
   public:
    struct ToolCallReviewResult
    {
        agent::Action proposedAction;
        bool allowed = false;
        QString detail;
    };

    struct ActionResult
    {
        enum class Type
        {
            CallTool,
            TaskCompleted,
            Blocked,
            Continue,
            Invalid
        };

        enum class Repair
        {
            General,
            ToolCallReview,
            PrematureFinal
        };

        Type type = Type::Invalid;
        Repair repair = Repair::General;
        agent::Action toolAction;
        QString content;
        QString blockReason;
        QString errorMessage;
        int evidenceEnd = 0;
        bool toolCallAlreadyRecorded = false;
    };

    explicit ExecutionTask(QJsonObject specification = {},
                           QSet<QString> toolsRequiringSemanticReview = {});
    explicit ExecutionTask(QString directDescription);

    [[nodiscard]] const QJsonObject& specification() const;
    [[nodiscard]] QJsonObject completionSnapshot() const;
    [[nodiscard]] Directive completeTaskGeneration(
        bool cancelled, const QList<QJsonObject>& toolEvidence,
        const QString& unresolvedVerificationReason) override;

    void activate(QList<chat::Message> messageSeed,
                  const QJsonArray& completedSteps,
                  const QList<QJsonObject>& priorToolEvidence,
                  int evidenceStart);
    void activateDirect(QList<chat::Message> messages);
    [[nodiscard]] bool requiresTool() const;
    [[nodiscard]] ActionResult handleAction(
        const agent::Action& action, const QByteArray& rawAction,
        const QList<QJsonObject>& toolEvidence,
        const QString& unresolvedVerificationReason);
    [[nodiscard]] QString validateToolAction(const agent::Action& action,
                                             bool toolIsReadOnly) const;

    void awaitTool();
    void awaitApproval();
    void receiveToolResult(const agent::ToolResult& result,
                           int evidenceSequence, const QJsonObject& ledgerState,
                           const QString& verificationReason);
    [[nodiscard]] bool repairAction(const QByteArray& rawAction,
                                    const QString& errorMessage,
                                    bool recordAction = true);
    void markBlocked();

    [[nodiscard]] int evidenceStart() const;
    [[nodiscard]] int evidenceEnd() const;

    [[nodiscard]] bool hasPendingToolCallReview() const;

   private:
    [[nodiscard]] QString activity() const override;
    void beginToolCallReview(const agent::Action& action,
                             const QByteArray& rawAction);
    [[nodiscard]] bool repairToolCallReview(const QByteArray& rawAction,
                                            const QString& errorMessage);
    [[nodiscard]] bool repairPrematureFinal(const QByteArray& rawAction,
                                            const QString& errorMessage);
    [[nodiscard]] std::optional<ToolCallReviewResult> resolveToolCallReview(
        const agent::Action& review, const QByteArray& rawAction);
    [[nodiscard]] bool completeWithoutTool(const agent::Action& action,
                                           const QByteArray& rawAction);
    [[nodiscard]] QString completeWithEvidence(
        const agent::Action& action, const QByteArray& rawAction,
        const QList<QJsonObject>& toolEvidence,
        const QString& unresolvedVerificationReason);
    void markSatisfied(const QJsonArray& evidence);

    QJsonObject specification_;
    bool managedPlan_ = true;
    QSet<QString> toolsRequiringSemanticReview_;
    int evidenceStart_ = 1;
    int evidenceEnd_ = 0;
    QJsonArray evidence_;
    QString output_;
    std::optional<agent::Action> pendingToolCallReview_;
    int toolCallReviewFailures_ = 0;
    int actionRepairFailures_ = 0;
    int prematureFinalFailures_ = 0;
};
}  // namespace qtllm::application
