#include "PlanTask.hpp"

#include "AgentPromptBuilder.hpp"

#include <QJsonDocument>

#include <algorithm>
#include <utility>

namespace qtllm::application
{
PlanTask::PlanTask(QJsonObject specification)
    : AgentTask(Kind::Execution,
                specification.value(QStringLiteral("id")).toString(),
                specification.value(QStringLiteral("description")).toString()),
      specification_(std::move(specification))
{
}

const QJsonObject& PlanTask::specification() const
{
    return specification_;
}

QJsonObject PlanTask::completionSnapshot() const
{
    auto result = specification_;
    switch (status())
    {
        case Status::Completed:
            result.insert(QStringLiteral("status"),
                          QStringLiteral("satisfied"));
            result.insert(QStringLiteral("evidence"), evidence_);
            break;
        case Status::Blocked:
            result.insert(QStringLiteral("status"), QStringLiteral("blocked"));
            result.insert(QStringLiteral("evidence"), evidence_);
            break;
        case Status::Cancelled:
            result.insert(QStringLiteral("status"),
                          QStringLiteral("cancelled"));
            result.insert(QStringLiteral("evidence"), evidence_);
            break;
        case Status::Failed:
            result.insert(QStringLiteral("status"), QStringLiteral("failed"));
            result.insert(QStringLiteral("evidence"), evidence_);
            break;
        default:
            result.remove(QStringLiteral("status"));
            result.remove(QStringLiteral("evidence"));
            break;
    }
    return result;
}

void PlanTask::activate(QList<chat::Message> messageSeed,
                        const QJsonArray& completedSteps,
                        const QList<QJsonObject>& priorToolEvidence,
                        int evidenceStart)
{
    messageSeed.append(AgentPromptBuilder::planTaskActivationMessage(
        specification_, completedSteps, priorToolEvidence));
    const auto requestMessageIndex = messageSeed.size() - 1;
    evidenceStart_ = evidenceStart;
    evidenceEnd_ = 0;
    evidence_ = {};
    awaitingStepReview_ = false;
    pendingToolCallReview_.reset();
    actionRepairFailures_ = 0;
    prematureFinalFailures_ = 0;
    finalAnswerPrepared_ = false;
    activateConversation(std::move(messageSeed), requestMessageIndex);
}

bool PlanTask::requiresTool() const
{
    return specification_.value(QStringLiteral("requires_tool")).toBool();
}

PlanTask::ActionResult PlanTask::handleAction(
    const agent::Action& action, const QByteArray& rawAction,
    const QList<QJsonObject>& toolEvidence,
    const QString& unresolvedVerificationReason)
{
    ActionResult result;
    if (hasPendingToolCallReview())
    {
        if (action.type != agent::ActionType::ReviewToolCall)
        {
            result.repair = ActionResult::Repair::ToolCallReview;
            result.errorMessage = QStringLiteral(
                "The pending tool call must be reviewed before another "
                "action. Return review_tool_call now.");
            return result;
        }
        const auto review = resolveToolCallReview(action, rawAction);
        if (!review.has_value())
        {
            result.repair = ActionResult::Repair::ToolCallReview;
            result.errorMessage =
                QStringLiteral("No proposed tool call is awaiting review.");
            return result;
        }
        if (!review->allowed)
        {
            result.type = ActionResult::Type::Continue;
            return result;
        }
        result.type = ActionResult::Type::CallTool;
        result.toolAction = review->proposedAction;
        result.toolCallAlreadyRecorded = true;
        return result;
    }
    if (action.type == agent::ActionType::ReviewToolCall)
    {
        result.errorMessage = QStringLiteral(
            "review_tool_call is valid only when this task requested it.");
        return result;
    }
    if (action.type == agent::ActionType::Blocked)
    {
        result.type = ActionResult::Type::Blocked;
        result.blockReason = action.blockReason;
        result.content = action.content;
        return result;
    }
    if (finalAnswerPrepared_ && action.type != agent::ActionType::Final)
    {
        result.errorMessage = QStringLiteral(
            "All tasks are verified. Return final without another tool or "
            "review action.");
        return result;
    }

    if (awaitingReview())
    {
        if (action.type != agent::ActionType::ReviewPlanStep)
        {
            result.repair = ActionResult::Repair::StepReview;
            result.errorMessage = QStringLiteral(
                "This task result must be reviewed before another action. "
                "Return review_plan_step now.");
            return result;
        }
        const auto review = reviewStep(action, rawAction, toolEvidence,
                                       unresolvedVerificationReason);
        if (review.status == StepReviewResult::Status::Invalid)
        {
            result.repair = ActionResult::Repair::StepReview;
            result.errorMessage = review.errorMessage;
            return result;
        }
        if (review.status == StepReviewResult::Status::Pending)
        {
            result.type = ActionResult::Type::Continue;
            return result;
        }
        result.type = ActionResult::Type::TaskCompleted;
        result.evidenceEnd = evidenceEnd_;
        return result;
    }
    if (action.type == agent::ActionType::ReviewPlanStep)
    {
        result.errorMessage = QStringLiteral(
            "review_plan_step is valid only after this task requested it.");
        return result;
    }

    if (action.type == agent::ActionType::Final)
    {
        if (finalAnswerPrepared_)
        {
            result.type = ActionResult::Type::FinalAnswer;
            result.content = action.content;
            return result;
        }
        if (!completeWithoutTool(rawAction))
        {
            result.repair = ActionResult::Repair::PrematureFinal;
            result.errorMessage = QStringLiteral(
                "This task requires tool evidence before it can complete.");
            return result;
        }
        result.type = ActionResult::Type::TaskCompleted;
        result.content = action.content;
        return result;
    }
    if (action.type == agent::ActionType::CallTool)
    {
        result.type = ActionResult::Type::CallTool;
        result.toolAction = action;
        return result;
    }

    result.errorMessage =
        QStringLiteral("This action is not valid for the active task.");
    return result;
}

QString PlanTask::validateToolAction(const agent::Action& action,
                                     bool toolIsReadOnly) const
{
    const auto stepId = id();
    if (action.planStepId.isEmpty() || !action.completesPlanStep.has_value())
    {
        const auto missing = action.planStepId.isEmpty()
                                 ? QStringLiteral("plan_step_id")
                                 : QStringLiteral("completes_plan_step");
        return QStringLiteral(
                   "The current task is '%1'. The call is missing %2. Use "
                   "plan_step_id='%1' and set completes_plan_step=true only "
                   "for the final call that finishes this task.")
            .arg(stepId, missing);
    }
    if (action.planStepId != stepId)
        return QStringLiteral(
                   "This call targets plan_step_id '%1', but this task worker "
                   "owns only '%2' (%3).")
            .arg(
                action.planStepId, stepId,
                specification_.value(QStringLiteral("description")).toString());
    if (!requiresTool())
        return QStringLiteral(
                   "The current task '%1' does not require a tool. Complete "
                   "this task with a final action.")
            .arg(
                specification_.value(QStringLiteral("description")).toString());

    const auto allowedTools =
        specification_.value(QStringLiteral("allowed_tools")).toArray();
    if (!toolIsReadOnly && !allowedTools.contains(action.toolName))
        return QStringLiteral(
                   "Tool '%1' is not assigned to current task '%2' (%3). Its "
                   "allowed_tools are %4. Only a directly relevant read-only "
                   "inspection or recovery tool may be unlisted.")
            .arg(action.toolName, stepId,
                 specification_.value(QStringLiteral("description")).toString(),
                 QString::fromUtf8(QJsonDocument(allowedTools)
                                       .toJson(QJsonDocument::Compact)));
    return {};
}

void PlanTask::awaitTool()
{
    setStatus(Status::WaitingForTool);
}

void PlanTask::awaitApproval()
{
    setStatus(Status::WaitingForApproval);
}

void PlanTask::beginReview(int evidenceEnd)
{
    evidenceEnd_ = evidenceEnd;
    planStepReviewFailures_ = 0;
    awaitingStepReview_ = true;
    setStatus(Status::Running);
}

void PlanTask::appendToolResult(chat::Message resultMessage,
                                const QList<QJsonObject>& toolEvidence,
                                const QJsonObject& ledgerState,
                                const QString& verificationReason)
{
    if (awaitingReview())
    {
        const auto reviewMessage = AgentPromptBuilder::planStepReviewMessage(
            specification_, toolEvidence, evidenceStart_, evidenceEnd_,
            ledgerState, verificationReason);
        resultMessage.content += QStringLiteral("\n\n") + reviewMessage.content;
    }
    messages().append(std::move(resultMessage));
}

void PlanTask::beginToolCallReview(const agent::Action& action,
                                   const QByteArray& rawAction)
{
    pendingToolCallReview_ = action;
    toolCallReviewFailures_ = 0;
    setStatus(Status::Running);
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    messages().append(
        AgentPromptBuilder::toolCallReviewMessage(specification_, action));
}

std::optional<PlanTask::ToolCallReviewResult> PlanTask::resolveToolCallReview(
    const agent::Action& review, const QByteArray& rawAction)
{
    if (!pendingToolCallReview_.has_value()) return std::nullopt;
    ToolCallReviewResult result{
        *pendingToolCallReview_,
        review.toolReviewVerdict == QLatin1String("allow"),
        review.toolReviewDetail};
    pendingToolCallReview_.reset();
    toolCallReviewFailures_ = 0;
    setStatus(Status::Running);
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    if (!result.allowed)
        messages().append(AgentPromptBuilder::toolCallReviewContinuationMessage(
            specification_, result.detail));
    return result;
}

bool PlanTask::repairToolCallReview(const QByteArray& rawAction,
                                    const QString& errorMessage)
{
    if (!pendingToolCallReview_.has_value() || toolCallReviewFailures_ >= 1)
        return false;
    ++toolCallReviewFailures_;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    auto correction = AgentPromptBuilder::toolCallReviewMessage(
        specification_, *pendingToolCallReview_);
    correction.content.prepend(
        QStringLiteral("The prior current-task call review was invalid: %1 ")
            .arg(errorMessage));
    messages().append(std::move(correction));
    return true;
}

PlanTask::StepReviewResult PlanTask::reviewStep(
    const agent::Action& review, const QByteArray& rawAction,
    const QList<QJsonObject>& toolEvidence,
    const QString& unresolvedVerificationReason)
{
    StepReviewResult result;
    if (!awaitingReview() || evidenceEnd_ <= 0)
    {
        result.errorMessage =
            QStringLiteral("No result for this task is awaiting review.");
        return result;
    }
    if (review.planStepId != id())
    {
        result.errorMessage =
            QStringLiteral(
                "review_plan_step targets '%1', but this task is "
                "'%2'.")
                .arg(review.planStepId, id());
        return result;
    }

    auto citesTerminalResult = false;
    for (const auto& value : review.planStepReviewEvidence)
    {
        const auto sequence = value.toInt();
        if (sequence < evidenceStart_ || sequence > evidenceEnd_)
        {
            result.errorMessage =
                QStringLiteral(
                    "Evidence %1 is outside this task's evidence "
                    "range %2 through %3.")
                    .arg(sequence)
                    .arg(evidenceStart_)
                    .arg(evidenceEnd_);
            return result;
        }
        const auto evidence = std::find_if(
            toolEvidence.cbegin(), toolEvidence.cend(),
            [sequence](const QJsonObject& candidate)
            {
                return candidate.value(QStringLiteral("sequence")).toInt() ==
                       sequence;
            });
        if (evidence == toolEvidence.cend())
        {
            result.errorMessage =
                QStringLiteral("Unknown tool evidence sequence %1.")
                    .arg(sequence);
            return result;
        }
        citesTerminalResult =
            citesTerminalResult ||
            (evidence->value(QStringLiteral("outcome")).toString() ==
                 QLatin1String("success") &&
             evidence->value(QStringLiteral("terminal")).toBool());
    }
    if (review.planStepReviewStatus == QLatin1String("satisfied"))
    {
        if (!citesTerminalResult)
        {
            result.errorMessage = QStringLiteral(
                "A satisfied review must cite successful terminal evidence "
                "from this task's evidence range.");
            return result;
        }
        if (!unresolvedVerificationReason.isEmpty())
        {
            result.errorMessage = unresolvedVerificationReason;
            return result;
        }
    }

    planStepReviewFailures_ = 0;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    result.detail = review.completionDetail;
    result.evidence = review.planStepReviewEvidence;
    if (review.planStepReviewStatus == QLatin1String("pending"))
    {
        result.status = StepReviewResult::Status::Pending;
        markPending();
        messages().append(AgentPromptBuilder::planStepContinuationMessage(
            specification_, result.detail));
        return result;
    }

    result.status = StepReviewResult::Status::Satisfied;
    markSatisfied(result.evidence);
    return result;
}

bool PlanTask::repairStepReview(const QByteArray& rawAction,
                                const QString& errorMessage)
{
    if (planStepReviewFailures_ >= 1) return false;
    ++planStepReviewFailures_;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    messages().append(AgentPromptBuilder::planStepReviewCorrectionMessage(
        errorMessage, specification_));
    return true;
}

bool PlanTask::repairAction(const QByteArray& rawAction,
                            const QString& errorMessage)
{
    constexpr auto maximumRepairs = 3;
    if (actionRepairFailures_ >= maximumRepairs) return false;
    ++actionRepairFailures_;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    messages().append(
        AgentPromptBuilder::orderedPlanCorrectionMessage(errorMessage));
    return true;
}

bool PlanTask::repairPrematureFinal(const QByteArray& rawAction,
                                    const QString& errorMessage)
{
    if (prematureFinalFailures_ >= 1) return false;
    ++prematureFinalFailures_;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    messages().append(AgentPromptBuilder::unfinishedFinalMessage(errorMessage));
    return true;
}

bool PlanTask::completeWithoutTool(const QByteArray& rawAction)
{
    if (requiresTool()) return false;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    markSatisfied({});
    return true;
}

void PlanTask::prepareFinalAnswer(const QJsonArray& steps,
                                  const QList<QJsonObject>& toolEvidence)
{
    finalAnswerPrepared_ = true;
    messages().append(
        AgentPromptBuilder::allPlanTasksCompletedMessage(steps, toolEvidence));
}

void PlanTask::markPending()
{
    evidenceEnd_ = 0;
    awaitingStepReview_ = false;
    setStatus(Status::Running);
}

void PlanTask::markSatisfied(const QJsonArray& evidence)
{
    evidence_ = evidence;
    pendingToolCallReview_.reset();
    awaitingStepReview_ = false;
    complete();
}

void PlanTask::markBlocked()
{
    pendingToolCallReview_.reset();
    awaitingStepReview_ = false;
    block();
}

bool PlanTask::awaitingReview() const
{
    return awaitingStepReview_;
}

int PlanTask::evidenceStart() const
{
    return evidenceStart_;
}

int PlanTask::evidenceEnd() const
{
    return evidenceEnd_;
}

bool PlanTask::hasPendingToolCallReview() const
{
    return pendingToolCallReview_.has_value();
}

QString PlanTask::activity() const
{
    if (finalAnswerPrepared_)
        return QStringLiteral("Preparing the final answer");
    if (awaitingStepReview_)
        return QStringLiteral("Reviewing the current task result");
    if (pendingToolCallReview_.has_value())
        return QStringLiteral("Checking the current task boundary");
    switch (status())
    {
        case Status::Pending:
            return QStringLiteral("Waiting to start");
        case Status::Running:
        case Status::WaitingForModel:
            return QStringLiteral("Working on the current task");
        case Status::WaitingForApproval:
            return QStringLiteral("Waiting for approval");
        case Status::WaitingForTool:
            return QStringLiteral("Waiting for the tool result");
        case Status::Completed:
            return QStringLiteral("Task completed");
        case Status::Blocked:
            return QStringLiteral("Task blocked");
        case Status::Cancelled:
            return QStringLiteral("Task stopped");
        case Status::Failed:
            return QStringLiteral("Task failed");
    }
    return {};
}
}  // namespace qtllm::application
