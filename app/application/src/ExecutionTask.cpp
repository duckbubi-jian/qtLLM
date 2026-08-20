#include "ExecutionTask.hpp"

#include "AgentPromptBuilder.hpp"

#include <QJsonDocument>

#include <utility>

namespace qtllm::application
{
ExecutionTask::ExecutionTask(QJsonObject specification)
    : AgentTask(Kind::Execution,
                specification.value(QStringLiteral("id")).toString(),
                specification.value(QStringLiteral("description")).toString()),
      specification_(std::move(specification))
{
}

ExecutionTask::ExecutionTask(QString directDescription)
    : AgentTask(Kind::Execution, QStringLiteral("direct"), directDescription),
      specification_({
          {QStringLiteral("id"), QStringLiteral("direct")},
          {QStringLiteral("description"), std::move(directDescription)},
          {QStringLiteral("requires_tool"), false},
          {QStringLiteral("allowed_tools"), QJsonArray{}},
      }),
      managedPlan_(false)
{
}

const QJsonObject& ExecutionTask::specification() const
{
    return specification_;
}

QJsonObject ExecutionTask::completionSnapshot() const
{
    auto result = specification_;
    if (!output_.isEmpty())
        result.insert(QStringLiteral("output"), output_);
    else
        result.remove(QStringLiteral("output"));
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

void ExecutionTask::activate(QList<chat::Message> messageSeed,
                             const QJsonArray& completedSteps,
                             const QList<QJsonObject>& priorToolEvidence,
                             int evidenceStart, const QString& originalRequest)
{
    messageSeed.append(AgentPromptBuilder::planTaskActivationMessage(
        specification_, completedSteps, priorToolEvidence, originalRequest));
    const auto requestMessageIndex = messageSeed.size() - 1;
    evidenceStart_ = evidenceStart;
    evidenceEnd_ = 0;
    evidence_ = {};
    userResolvedEvidence_ = {};
    output_.clear();
    pendingVerificationAction_.clear();
    actionRepairFailures_ = 0;
    prematureFinalFailures_ = 0;
    activateConversation(std::move(messageSeed), requestMessageIndex);
}

void ExecutionTask::activateDirect(QList<chat::Message> messages)
{
    const auto requestMessageIndex = messages.size() - 1;
    evidenceStart_ = 1;
    evidenceEnd_ = 0;
    evidence_ = {};
    userResolvedEvidence_ = {};
    output_.clear();
    pendingVerificationAction_.clear();
    actionRepairFailures_ = 0;
    prematureFinalFailures_ = 0;
    activateConversation(std::move(messages), requestMessageIndex);
}

bool ExecutionTask::requiresTool() const
{
    return specification_.value(QStringLiteral("requires_tool")).toBool();
}

AgentTask::Directive ExecutionTask::completeTaskGeneration(
    bool cancelled, const QList<QJsonObject>& toolEvidence,
    const QString& unresolvedVerificationReason)
{
    const auto decision = completeDecision(cancelled);
    const auto repair = [this](const QByteArray& rawAction,
                               const QString& errorMessage,
                               ActionResult::Repair repairType)
    {
        auto repaired = false;
        auto failureCode = QStringLiteral("invalid_agent_action");
        auto activity = QStringLiteral("Correcting the current task action");
        switch (repairType)
        {
            case ActionResult::Repair::PrematureFinal:
                repaired = repairPrematureFinal(rawAction, errorMessage);
                failureCode = QStringLiteral("completion_unverified");
                activity = QStringLiteral("Continuing unfinished task work");
                break;
            case ActionResult::Repair::General:
                repaired = repairAction(rawAction, errorMessage);
                break;
        }

        Directive directive;
        directive.type =
            repaired ? Directive::Type::Generate : Directive::Type::Failed;
        if (!repaired) directive.code = failureCode;
        directive.detail = errorMessage;
        directive.activity = activity;
        if (!repaired) fail();
        return directive;
    };

    if (!decision.valid)
    {
        return repair(decision.rawAction, decision.errorMessage,
                      ActionResult::Repair::General);
    }

    if (!managedPlan_)
    {
        Directive directive;
        directive.rawAction = decision.rawAction;
        if (decision.action.type == agent::ActionType::Blocked)
        {
            markBlocked();
            directive.type = Directive::Type::Blocked;
            directive.code = decision.action.blockReason;
            directive.content = decision.action.content;
            return directive;
        }
        if (decision.action.type == agent::ActionType::Final)
        {
            auto hasToolEvidence = false;
            for (const auto& evidence : toolEvidence)
            {
                if (evidence.value(QStringLiteral("sequence")).toInt() >=
                    evidenceStart_)
                {
                    hasToolEvidence = true;
                    break;
                }
            }
            if (hasToolEvidence)
            {
                if (!unresolvedVerificationReason.isEmpty())
                {
                    pendingVerificationAction_ = decision.rawAction;
                    awaitVerification();
                    directive.type = Directive::Type::VerificationRequired;
                    directive.content = decision.action.content;
                    directive.detail = unresolvedVerificationReason;
                    return directive;
                }
                const auto completionError = completeWithEvidence(
                    decision.action, decision.rawAction, toolEvidence,
                    unresolvedVerificationReason);
                if (!completionError.isEmpty())
                    return repair(decision.rawAction, completionError,
                                  ActionResult::Repair::PrematureFinal);
                directive.type = Directive::Type::Completed;
                directive.content = output_;
                directive.evidenceEnd = evidenceEnd_;
                return directive;
            }
            if (!unresolvedVerificationReason.isEmpty())
            {
                pendingVerificationAction_ = decision.rawAction;
                awaitVerification();
                directive.type = Directive::Type::VerificationRequired;
                directive.content = decision.action.content;
                directive.detail = unresolvedVerificationReason;
                return directive;
            }
            messages().append(
                {chat::Role::Assistant, QString::fromUtf8(decision.rawAction)});
            output_ = decision.action.content;
            complete();
            directive.type = Directive::Type::Completed;
            directive.content = output_;
            return directive;
        }
        if (decision.action.type == agent::ActionType::CallTool)
        {
            directive.type = Directive::Type::CallTool;
            directive.toolAction = decision.action;
            return directive;
        }
        return repair(
            decision.rawAction,
            QStringLiteral(
                "This direct task accepts one call_tool, blocked, or final "
                "action. It does not accept task-plan or review actions."),
            ActionResult::Repair::General);
    }

    const auto result =
        handleAction(decision.action, decision.rawAction, toolEvidence,
                     unresolvedVerificationReason);
    Directive directive;
    directive.rawAction = decision.rawAction;
    switch (result.type)
    {
        case ActionResult::Type::CallTool:
            directive.type = Directive::Type::CallTool;
            directive.toolAction = result.toolAction;
            return directive;
        case ActionResult::Type::TaskCompleted:
            directive.type = Directive::Type::Completed;
            directive.content = result.content;
            directive.evidenceEnd = result.evidenceEnd;
            return directive;
        case ActionResult::Type::Blocked:
            markBlocked();
            directive.type = Directive::Type::Blocked;
            directive.code = result.blockReason;
            directive.content = result.content;
            return directive;
        case ActionResult::Type::VerificationRequired:
            pendingVerificationAction_ = decision.rawAction;
            awaitVerification();
            directive.type = Directive::Type::VerificationRequired;
            directive.content = result.content;
            directive.detail = result.errorMessage;
            return directive;
        case ActionResult::Type::Continue:
            directive.type = Directive::Type::Continue;
            directive.activity = QStringLiteral("Continuing the current task");
            return directive;
        case ActionResult::Type::Invalid:
            return repair(decision.rawAction, result.errorMessage,
                          result.repair);
    }
    return directive;
}

ExecutionTask::ActionResult ExecutionTask::handleAction(
    const agent::Action& action, const QByteArray& rawAction,
    const QList<QJsonObject>& toolEvidence,
    const QString& unresolvedVerificationReason)
{
    ActionResult result;
    if (action.type == agent::ActionType::Blocked)
    {
        result.type = ActionResult::Type::Blocked;
        result.blockReason = action.blockReason;
        result.content = action.content;
        return result;
    }
    if (action.type == agent::ActionType::Final)
    {
        if (requiresTool())
        {
            if (!unresolvedVerificationReason.isEmpty())
            {
                result.type = ActionResult::Type::VerificationRequired;
                result.content = action.content;
                result.errorMessage = unresolvedVerificationReason;
                return result;
            }
            const auto completionError = completeWithEvidence(
                action, rawAction, toolEvidence, unresolvedVerificationReason);
            if (!completionError.isEmpty())
            {
                result.repair = ActionResult::Repair::PrematureFinal;
                result.errorMessage = completionError;
                return result;
            }
            result.type = ActionResult::Type::TaskCompleted;
            result.content = action.content;
            result.evidenceEnd = evidenceEnd_;
            return result;
        }
        if (!completeWithoutTool(action, rawAction))
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

QString ExecutionTask::validateToolAction(const agent::Action& action,
                                          bool toolIsReadOnly) const
{
    const auto stepId = id();
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

void ExecutionTask::awaitTool()
{
    setStatus(Status::WaitingForTool);
}

void ExecutionTask::awaitApproval()
{
    setStatus(Status::WaitingForApproval);
}

void ExecutionTask::awaitVerification()
{
    setStatus(Status::WaitingForVerification);
}

bool ExecutionTask::resumeWithUserEvidence(const QString& evidence)
{
    const auto userEvidence = evidence.trimmed();
    if (userEvidence.isEmpty() || messages().isEmpty()) return false;

    if (!pendingVerificationAction_.isEmpty())
    {
        if (messages().constLast().role != chat::Role::User) return false;
        messages().append({chat::Role::Assistant,
                           QString::fromUtf8(pendingVerificationAction_)});
        pendingVerificationAction_.clear();
        messages().append(
            {chat::Role::User,
             QStringLiteral(
                 "<user_evidence>\n%1\n</user_evidence>\n"
                 "This is user-attested evidence, not deterministic Host "
                 "verification. Continue the current task without repeating "
                 "an uncertain mutation.")
                 .arg(userEvidence)});
    }
    else if (messages().constLast().role == chat::Role::User)
    {
        messages().last().content +=
            QStringLiteral(
                "\n\n<user_evidence>\n%1\n</user_evidence>\n"
                "This is user-attested evidence, not deterministic Host "
                "verification. Continue the current task without repeating "
                "an uncertain mutation.")
                .arg(userEvidence);
    }
    else
    {
        messages().append(
            {chat::Role::User,
             QStringLiteral(
                 "<user_evidence>\n%1\n</user_evidence>\n"
                 "This is user-attested evidence, not deterministic Host "
                 "verification.")
                 .arg(userEvidence)});
    }
    setStatus(Status::Running);
    return true;
}

void ExecutionTask::allowUnverifiedCompletion(
    const QList<int>& evidenceSequences)
{
    for (const auto sequence : evidenceSequences)
    {
        if (sequence < evidenceStart_ ||
            userResolvedEvidence_.contains(sequence))
            continue;
        userResolvedEvidence_.append(sequence);
    }
}

void ExecutionTask::receiveToolResult(const agent::ToolResult& result,
                                      int evidenceSequence,
                                      const QJsonObject& ledgerState,
                                      const QString& verificationReason)
{
    QString recoveryGuidance;
    const auto inProgress = result.outcome == agent::ToolOutcome::InProgress;
    if (evidenceSequence >= evidenceStart_)
        evidenceEnd_ = qMax(evidenceEnd_, evidenceSequence);
    if (managedPlan_ && !inProgress)
        recoveryGuidance = QStringLiteral(
            "This result is bound to the current task by the task "
            "runtime. Continue only with dependent work for this task, "
            "or return final when this task's requested outcome is "
            "complete. The runtime will validate and attach task "
            "evidence; do not work on a later task.");

    messages().append(AgentPromptBuilder::toolResultMessage(
        result, ledgerState, verificationReason, recoveryGuidance));
}

bool ExecutionTask::repairAction(const QByteArray& rawAction,
                                 const QString& errorMessage, bool recordAction)
{
    const auto maximumRepairs = managedPlan_ ? 3 : 1;
    if (actionRepairFailures_ >= maximumRepairs) return false;
    ++actionRepairFailures_;
    return appendCorrectionTurn(
        rawAction,
        managedPlan_
            ? AgentPromptBuilder::orderedPlanCorrectionMessage(errorMessage)
            : AgentPromptBuilder::correctionMessage(errorMessage),
        recordAction);
}

bool ExecutionTask::repairPrematureFinal(const QByteArray& rawAction,
                                         const QString& errorMessage)
{
    if (prematureFinalFailures_ >= 1) return false;
    ++prematureFinalFailures_;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    messages().append(AgentPromptBuilder::unfinishedFinalMessage(errorMessage));
    return true;
}

bool ExecutionTask::completeWithoutTool(const agent::Action& action,
                                        const QByteArray& rawAction)
{
    if (requiresTool()) return false;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    output_ = action.content;
    markSatisfied({});
    return true;
}

QString ExecutionTask::completeWithEvidence(
    const agent::Action& action, const QByteArray& rawAction,
    const QList<QJsonObject>& toolEvidence,
    const QString& unresolvedVerificationReason)
{
    if (!unresolvedVerificationReason.isEmpty())
        return unresolvedVerificationReason;

    QJsonArray successfulEvidence;
    const QJsonObject* latestEvidence = nullptr;
    for (const auto& evidence : toolEvidence)
    {
        const auto sequence =
            evidence.value(QStringLiteral("sequence")).toInt();
        if (sequence < evidenceStart_) continue;
        if (!latestEvidence ||
            sequence >
                latestEvidence->value(QStringLiteral("sequence")).toInt())
            latestEvidence = &evidence;
        if (evidence.value(QStringLiteral("outcome")).toString() ==
                QLatin1String("success") &&
            evidence.value(QStringLiteral("terminal")).toBool())
            successfulEvidence.append(sequence);
    }
    if (!latestEvidence ||
        (successfulEvidence.isEmpty() && userResolvedEvidence_.isEmpty()))
        return QStringLiteral(
            "The current task has no successful terminal tool result.");
    if (!latestEvidence->value(QStringLiteral("terminal")).toBool())
        return QStringLiteral(
            "The latest tool operation for the current task is still in "
            "progress.");

    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    pendingVerificationAction_.clear();
    evidenceEnd_ = latestEvidence->value(QStringLiteral("sequence")).toInt();
    output_ = action.content;
    if (successfulEvidence.isEmpty())
        successfulEvidence = userResolvedEvidence_;
    markSatisfied(successfulEvidence);
    return {};
}

void ExecutionTask::markSatisfied(const QJsonArray& evidence)
{
    evidence_ = evidence;
    complete();
}

void ExecutionTask::markBlocked()
{
    block();
}

int ExecutionTask::evidenceStart() const
{
    return evidenceStart_;
}

int ExecutionTask::evidenceEnd() const
{
    return evidenceEnd_;
}

QString ExecutionTask::activity() const
{
    switch (status())
    {
        case Status::Pending:
            return QStringLiteral("Waiting to start");
        case Status::Running:
            return QStringLiteral("Working on the current task");
        case Status::WaitingForModel:
            return QStringLiteral("Deciding the next action");
        case Status::WaitingForApproval:
            return QStringLiteral("Waiting for approval");
        case Status::WaitingForVerification:
            return QStringLiteral("Waiting for verification confirmation");
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
