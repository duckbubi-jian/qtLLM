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

    struct StepReviewResult
    {
        enum class Status
        {
            Invalid,
            Pending,
            Satisfied
        };

        Status status = Status::Invalid;
        QString errorMessage;
        QString detail;
        QJsonArray evidence;
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
            StepReview,
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
    void receiveToolResult(const agent::Action& action,
                           const agent::ToolResult& result,
                           int evidenceSequence,
                           const QList<QJsonObject>& toolEvidence,
                           const QJsonObject& ledgerState,
                           const QString& verificationReason);
    [[nodiscard]] bool repairAction(const QByteArray& rawAction,
                                    const QString& errorMessage);
    void markBlocked();

    [[nodiscard]] bool awaitingReview() const;
    [[nodiscard]] int evidenceStart() const;
    [[nodiscard]] int evidenceEnd() const;
    [[nodiscard]] int stepReviewAttempts() const;
    [[nodiscard]] int stepReviewSuccesses() const;

    [[nodiscard]] bool hasPendingToolCallReview() const;

   private:
    [[nodiscard]] QString activity() const override;
    void beginReview(int evidenceEnd);
    void beginToolCallReview(const agent::Action& action,
                             const QByteArray& rawAction);
    [[nodiscard]] bool repairToolCallReview(const QByteArray& rawAction,
                                            const QString& errorMessage);
    [[nodiscard]] bool repairStepReview(const QByteArray& rawAction,
                                        const QString& errorMessage);
    [[nodiscard]] bool repairPrematureFinal(const QByteArray& rawAction,
                                            const QString& errorMessage);
    [[nodiscard]] std::optional<ToolCallReviewResult> resolveToolCallReview(
        const agent::Action& review, const QByteArray& rawAction);
    [[nodiscard]] StepReviewResult reviewStep(
        const agent::Action& review, const QByteArray& rawAction,
        const QList<QJsonObject>& toolEvidence,
        const QString& unresolvedVerificationReason);
    [[nodiscard]] bool completeWithoutTool(const agent::Action& action,
                                           const QByteArray& rawAction);
    void markPending();
    void markSatisfied(const QJsonArray& evidence);

    QJsonObject specification_;
    bool managedPlan_ = true;
    QSet<QString> toolsRequiringSemanticReview_;
    int evidenceStart_ = 1;
    int evidenceEnd_ = 0;
    QJsonArray evidence_;
    QString output_;
    bool awaitingStepReview_ = false;
    std::optional<agent::Action> pendingToolCallReview_;
    int planStepReviewFailures_ = 0;
    int stepReviewAttempts_ = 0;
    int stepReviewSuccesses_ = 0;
    int toolCallReviewFailures_ = 0;
    int actionRepairFailures_ = 0;
    int prematureFinalFailures_ = 0;
};
}  // namespace qtllm::application
