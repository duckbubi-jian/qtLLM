#include "AgentController.hpp"

#include "AgentContextCompactor.hpp"
#include "AgentPromptBuilder.hpp"
#include "AgentRunMetrics.hpp"
#include "ToolResultStatus.hpp"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace qtllm::application
{
namespace
{
constexpr auto maximumDecisionBytes = 65'536;
constexpr auto minimumDecisionTokens = 256;
constexpr auto runTimeoutMilliseconds = 120'000;
constexpr auto maximumConsecutiveDiscoveryCalls = 4;
constexpr qsizetype maximumLoggedEventDataBytes = 4'096;
constexpr qsizetype maximumProgressActivities = 8;

bool explicitlyRequestsExhaustiveDiscovery(const QString& request)
{
    const auto normalized = request.trimmed().toLower();
    static const QRegularExpression englishTerms(QStringLiteral(
        R"(\b(all|every|everything|entire|exhaustive)\b|complete\s+inventory)"));
    if (englishTerms.match(normalized).hasMatch()) return true;

    static const QStringList terms{QStringLiteral("\u5168\u90e8"),
                                   QStringLiteral("\u6240\u6709"),
                                   QStringLiteral("\u9010\u9879"),
                                   QStringLiteral("\u5b8c\u6574\u76d8\u70b9")};
    return std::any_of(terms.cbegin(), terms.cend(),
                       [&normalized](const QString& term)
                       { return normalized.contains(term); });
}

QString eventDataSummary(const QJsonObject& data)
{
    auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (bytes.size() > maximumLoggedEventDataBytes)
    {
        bytes = bytes.left(maximumLoggedEventDataBytes);
        bytes.append("...");
    }
    return QString::fromUtf8(bytes);
}

QString singleLine(QString value)
{
    return value.replace(QLatin1Char('\r'), QLatin1Char(' '))
        .replace(QLatin1Char('\n'), QLatin1Char(' '))
        .trimmed();
}

bool isDiscoveryToolName(const QString& qualifiedToolName)
{
    const auto name = qualifiedToolName.section(QLatin1Char('.'), -1).toLower();
    static const QStringList verbs{
        QStringLiteral("list"),     QStringLiteral("get"),
        QStringLiteral("describe"), QStringLiteral("inspect"),
        QStringLiteral("find"),     QStringLiteral("query"),
        QStringLiteral("search"),   QStringLiteral("read"),
        QStringLiteral("fetch"),    QStringLiteral("lookup"),
        QStringLiteral("retrieve"), QStringLiteral("enumerate"),
        QStringLiteral("scan"),     QStringLiteral("view"),
        QStringLiteral("snapshot")};
    static const QRegularExpression separator(QStringLiteral("[^a-z0-9]+"));
    const auto tokens = name.split(separator, Qt::SkipEmptyParts);
    return std::any_of(tokens.cbegin(), tokens.cend(), [](const QString& token)
                       { return verbs.contains(token); });
}

qsizetype messageCharacters(const QList<chat::Message>& messages)
{
    qsizetype total = 0;
    for (const auto& message : messages)
        total += message.content.size();
    return total;
}
}  // namespace

AgentController::AgentController(Dependencies dependencies, QObject* parent)
    : QObject(parent),
      dependencies_(std::move(dependencies)),
      toolRuntime_({dependencies_.validateTool, dependencies_.toolPolicy,
                    dependencies_.toolRisk, dependencies_.callTool,
                    dependencies_.cancelTool}),
      runTimer_(new QTimer(this)),
      pollTimer_(new QTimer(this))
{
    qRegisterMetaType<AgentProgressSnapshot>();
    runTimer_->setSingleShot(true);
    runTimer_->setInterval(runTimeoutMilliseconds);
    connect(runTimer_, &QTimer::timeout, this,
            &AgentController::notifyLongRunning);
    pollTimer_->setSingleShot(true);
    connect(pollTimer_, &QTimer::timeout, this,
            &AgentController::executePendingPoll);
}

AgentController::~AgentController() = default;

void AgentController::notifyLongRunning()
{
    if (!hasActiveRun()) return;
    recordEvent(
        agent::EventType::Warning,
        QStringLiteral("Agent is still running after two minutes. It will "
                       "continue until completion or Stop."));
}

bool AgentController::start(const QString& userRequest,
                            const models::InferencePreset& preset,
                            const QList<agent::ToolDefinition>& tools,
                            const AssistantContext& context)
{
    const auto request = userRequest.trimmed();
    if (request.isEmpty() || hasActiveRun() || !dependencies_.generate ||
        !toolRuntime_.isReady())
        return false;

    toolRuntime_.setTools(tools);

    AgentRun run;
    run.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    run.userRequest = request;
    run.startedAt = QDateTime::currentDateTimeUtc();
    run.inferenceMessages = AgentPromptBuilder::initialMessages(
        request, tools, conversationMessages_, context);
    run.requestMessageIndex = run.inferenceMessages.size() - 1;
    run.taskPlanRequired =
        !tools.isEmpty() &&
        AgentPromptBuilder::requiresCompletionReview(request);
    if (run.taskPlanRequired)
    {
        // Keep the initial prompt as one user turn; the worker requires
        // strictly alternating user and assistant messages.
        run.inferenceMessages.last().content +=
            QStringLiteral("\n\n") +
            AgentPromptBuilder::taskPlanMessage(request).content;
        QStringList availableToolNames;
        availableToolNames.reserve(tools.size());
        for (const auto& tool : tools)
            availableToolNames.append(tool.qualifiedName);
        run.planningTask.emplace(std::move(availableToolNames));
        run.planningTask->activate(run.inferenceMessages);
    }
    else
    {
        run.directTask.emplace(request);
        run.directTask->activateDirect(run.inferenceMessages);
    }
    activeRun_ = std::move(run);
    preset_ = preset;
    toolRuntime_.clearPendingApproval();
    toolRuntime_.clearTransportState();
    toolRuntime_.resetCallHistory();
    clearPendingVerification();
    toolEvidence_.clear();
    if (pollTimer_->isActive()) pollTimer_->stop();
    if (!runTimer_->isActive()) runTimer_->start();

    recordEvent(agent::EventType::RunStarted,
                QStringLiteral("Agent run started."));
    if (const auto* task = currentTask())
    {
        const auto directExecution = task->kind() == AgentTask::Kind::Execution;
        recordTaskStarted(*task, directExecution ? 1 : 0,
                          directExecution ? 1 : 0);
    }
    emit userRequestAccepted(activeRun_->id, request);
    requestDecision();
    return true;
}

void AgentController::cancel()
{
    if (!hasActiveRun()) return;
    const auto previousState = state_;
    activeRun_->finishCode = QStringLiteral("cancelled");
    activeRun_->finishMessage = QStringLiteral("Agent run cancelled.");
    if (auto* task = currentTask())
    {
        task->cancel();
        if (!activeRun_->executionTasks.empty())
            refreshExecutionTaskSnapshots();
    }
    setState(AgentRun::State::Cancelled);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    toolRuntime_.clearPendingApproval();
    toolRuntime_.cancelActiveCall();
    clearPendingVerification();
    if (previousState == AgentRun::State::Deciding &&
        dependencies_.cancelGeneration)
        dependencies_.cancelGeneration();
    recordEvent(agent::EventType::Cancelled,
                QStringLiteral("Agent run cancelled."));
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, QStringLiteral("cancelled"),
                     QStringLiteral("Agent run cancelled."));
}

void AgentController::resolveApproval(bool approved)
{
    if (!hasActiveRun() || state_ != AgentRun::State::WaitingForApproval ||
        !toolRuntime_.pendingApproval().has_value())
        return;
    const auto action = toolRuntime_.resolveApproval(approved);
    if (!approved)
    {
        failRun(QStringLiteral("approval_denied"),
                QStringLiteral("The requested tool call was rejected."));
        return;
    }
    executeTool(*action);
}

void AgentController::resolveVerification(VerificationDecision decision,
                                          const QString& userEvidence)
{
    if (!hasActiveRun() || state_ != AgentRun::State::WaitingForVerification ||
        !verificationContinuation_.has_value())
        return;

    const auto continuation = *verificationContinuation_;
    const auto taskId = verificationTaskId_;
    const auto reason = verificationReason_;
    const auto evidenceSequence =
        verificationContext(taskId)
            .value(QStringLiteral("mutationEvidenceSequence"))
            .toInt();
    if (decision == VerificationDecision::Stop)
    {
        recordEvent(agent::EventType::VerificationResolved,
                    QStringLiteral("User stopped the unverified run."), {},
                    {{QStringLiteral("decision"), QStringLiteral("stop")}});
        clearPendingVerification();
        blockRun(QStringLiteral("verification_rejected"), reason);
        return;
    }

    if (decision == VerificationDecision::ProvideEvidence)
    {
        const auto input = userEvidence.trimmed();
        if (input.isEmpty())
        {
            emit verificationRequested(activeRun_->id, reason,
                                       verificationContext(taskId));
            return;
        }
        if (continuation == VerificationContinuation::StartSummary)
        {
            auto& messages = activeRun_->inferenceMessages;
            if (messages.isEmpty() ||
                messages.constLast().role != chat::Role::User)
            {
                failRun(QStringLiteral("task_state_invalid"),
                        QStringLiteral(
                            "The run context cannot receive user evidence."));
                return;
            }
            messages.last().content +=
                QStringLiteral(
                    "\n\n<user_evidence>\n%1\n</user_evidence>\n"
                    "This is user-attested evidence, not deterministic Host "
                    "verification.")
                    .arg(input);
        }
        else
        {
            auto* task = currentExecutionTask();
            if (!task || !task->resumeWithUserEvidence(input))
            {
                emit verificationRequested(activeRun_->id, reason,
                                           verificationContext(taskId));
                return;
            }
        }
        const auto attested = activeRun_->ledger.attestUnresolvedVerification(
            taskId, evidenceSequence, input);
        if (continuation != VerificationContinuation::StartSummary)
        {
            if (auto* task = currentExecutionTask())
                task->allowUnverifiedCompletion(attested);
        }
        QJsonArray attestedEvidence;
        for (const auto sequence : attested)
            attestedEvidence.append(sequence);
        recordEvent(
            agent::EventType::VerificationResolved,
            QStringLiteral("User supplied temporary verification evidence."),
            {},
            {{QStringLiteral("decision"), QStringLiteral("provide_evidence")},
             {QStringLiteral("inputCharacters"), input.size()},
             {QStringLiteral("evidence"), attestedEvidence}});
        const auto nextReason =
            activeRun_->ledger.unresolvedVerificationReason(taskId);
        if (!nextReason.isEmpty())
        {
            clearPendingVerification();
            requestVerification(nextReason, continuation, taskId);
            return;
        }
        clearPendingVerification();
        if (continuation == VerificationContinuation::StartSummary)
            activateSummaryTask();
        requestDecision(QStringLiteral("Applying user-supplied evidence"));
        return;
    }

    const auto accepted = activeRun_->ledger.acceptUnresolvedVerification(
        taskId, evidenceSequence);
    QJsonArray acceptedEvidence;
    for (const auto sequence : accepted)
        acceptedEvidence.append(sequence);
    recordEvent(
        agent::EventType::VerificationResolved,
        QStringLiteral("User accepted the unverified result."), {},
        {{QStringLiteral("decision"), QStringLiteral("accept_unverified")},
         {QStringLiteral("evidence"), acceptedEvidence}});
    if (continuation != VerificationContinuation::StartSummary)
    {
        if (auto* task = currentExecutionTask())
            task->allowUnverifiedCompletion(accepted);
    }
    const auto nextReason =
        activeRun_->ledger.unresolvedVerificationReason(taskId);
    if (!nextReason.isEmpty())
    {
        clearPendingVerification();
        requestVerification(nextReason, continuation, taskId);
        return;
    }
    clearPendingVerification();

    switch (continuation)
    {
        case VerificationContinuation::RepeatCurrentDecision:
        {
            auto* task = currentExecutionTask();
            if (!task)
            {
                failRun(QStringLiteral("task_state_invalid"),
                        QStringLiteral(
                            "No active task can resume after verification."));
                return;
            }
            const auto directive = task->completeTaskGeneration(
                false, toolEvidence_,
                activeRun_->ledger.unresolvedVerificationReason(taskId));
            applyTaskDirective(directive);
            return;
        }
        case VerificationContinuation::ResumeDecision:
            requestDecision(
                QStringLiteral("Recovering from an uncertain tool outcome"));
            return;
        case VerificationContinuation::StartSummary:
            activateSummaryTask();
            requestDecision();
            return;
    }
}

bool AgentController::clearConversation()
{
    if (hasActiveRun()) return false;
    conversationMessages_.clear();
    emit conversationCleared();
    return true;
}

bool AgentController::setConversationMessages(QList<chat::Message> messages)
{
    if (hasActiveRun()) return false;
    conversationMessages_ = std::move(messages);
    return true;
}

AgentRun::State AgentController::state() const
{
    return state_;
}

bool AgentController::hasActiveRun() const
{
    return activeRun_.has_value() && !isTerminal(state_);
}

const std::optional<AgentRun>& AgentController::activeRun() const
{
    return activeRun_;
}

const QList<chat::Message>& AgentController::conversationMessages() const
{
    return conversationMessages_;
}

bool AgentController::hasConversation() const
{
    return !conversationMessages_.isEmpty();
}

AgentProgressSnapshot AgentController::progressSnapshot() const
{
    AgentProgressSnapshot snapshot;
    if (!activeRun_) return snapshot;

    const auto& run = *activeRun_;
    snapshot.runId = run.id;
    snapshot.state = run.state;
    snapshot.elapsedMilliseconds =
        qMax<qint64>(0, run.startedAt.msecsTo(QDateTime::currentDateTimeUtc()));
    snapshot.completedToolCount = run.successfulToolResults;
    snapshot.evidenceCount = static_cast<int>(toolEvidence_.size());
    snapshot.finishCode = run.finishCode;
    snapshot.finishMessage = run.finishMessage;

    const auto terminal = isTerminal(run.state);
    for (qsizetype index = 0; index < run.completionSteps.size(); ++index)
    {
        const auto object = run.completionSteps.at(index).toObject();
        AgentProgressStep step;
        step.id = object.value(QStringLiteral("id")).toString();
        step.description =
            object.value(QStringLiteral("description")).toString();
        if (index < static_cast<qsizetype>(run.executionTasks.size()))
        {
            const auto taskSnapshot =
                run.executionTasks.at(static_cast<std::size_t>(index))
                    .runtimeSnapshot();
            step.activity = taskSnapshot.activity;
            step.elapsedMilliseconds = taskSnapshot.elapsedMilliseconds;
            if (run.state == AgentRun::State::Deciding &&
                index == run.currentExecutionTaskIndex &&
                !run.decisionOperation.isEmpty())
                step.activity = run.decisionOperation;
        }
        const auto status =
            object.value(QStringLiteral("status")).toString().toLower();
        if (status == QLatin1String("satisfied"))
            step.status = AgentProgressStepStatus::Completed;
        else if (status == QLatin1String("blocked") ||
                 status == QLatin1String("cancelled") ||
                 status == QLatin1String("failed"))
            step.status = AgentProgressStepStatus::Blocked;
        else if (run.state == AgentRun::State::Completed && status.isEmpty())
            step.status = AgentProgressStepStatus::Completed;
        else if (!terminal && index == run.currentExecutionTaskIndex)
        {
            step.status = AgentProgressStepStatus::Current;
            snapshot.currentStepId = step.id;
        }
        snapshot.steps.append(std::move(step));
    }

    const auto toolOperation = [](const std::optional<agent::Action>& action)
    {
        return action.has_value()
                   ? QStringLiteral("Calling %1").arg(action->toolName)
                   : QString{};
    };
    switch (run.state)
    {
        case AgentRun::State::Idle:
            snapshot.operation = QStringLiteral("Starting agent run");
            snapshot.waitingReason = QStringLiteral("Preparing the task");
            break;
        case AgentRun::State::Deciding:
            if (!run.decisionOperation.isEmpty())
                snapshot.operation = run.decisionOperation;
            else if (const auto* task = currentTask())
                snapshot.operation = task->runtimeSnapshot().activity;
            else
                snapshot.operation = QStringLiteral("Working on current task");
            snapshot.waitingReason =
                QStringLiteral("Waiting for the model response");
            break;
        case AgentRun::State::WaitingForApproval:
            snapshot.operation = toolOperation(toolRuntime_.pendingApproval());
            snapshot.waitingReason =
                QStringLiteral("Waiting for your approval");
            break;
        case AgentRun::State::WaitingForVerification:
            snapshot.operation = QStringLiteral("Agent needs confirmation");
            snapshot.waitingReason = verificationReason_;
            break;
        case AgentRun::State::ExecutingTool:
            snapshot.operation = toolOperation(toolRuntime_.activeAction());
            if (snapshot.operation.isEmpty())
                snapshot.operation = toolOperation(toolRuntime_.pendingPoll());
            snapshot.waitingReason =
                toolRuntime_.pendingPoll().has_value()
                    ? QStringLiteral("Waiting for the next status check")
                    : QStringLiteral("Waiting for the tool result");
            break;
        case AgentRun::State::GeneratingAnswer:
            snapshot.operation = QStringLiteral("Preparing the final answer");
            break;
        case AgentRun::State::Completed:
            snapshot.operation = QStringLiteral("Agent run completed");
            break;
        case AgentRun::State::Blocked:
            snapshot.operation = QStringLiteral("Agent run blocked");
            break;
        case AgentRun::State::Cancelled:
            snapshot.operation = QStringLiteral("Agent run stopped");
            break;
        case AgentRun::State::Failed:
            snapshot.operation = QStringLiteral("Agent run failed");
            break;
    }

    const auto firstEvent =
        qMax<qsizetype>(0, run.events.size() - maximumProgressActivities);
    for (auto index = firstEvent; index < run.events.size(); ++index)
    {
        const auto& event = run.events.at(index);
        snapshot.recentActivity.append({event.type, event.message.left(240),
                                        event.toolName.left(160),
                                        event.timestamp});
    }
    return snapshot;
}

void AgentController::receiveToken(const QByteArray& bytes)
{
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding ||
        bytes.isEmpty())
        return;
    auto* task = currentTask();
    if (!task || !task->hasConversation())
    {
        failRun(
            QStringLiteral("task_state_invalid"),
            QStringLiteral("No active task can receive the model response."));
        return;
    }
    if (!task->receiveToken(bytes, maximumDecisionBytes))
        failRun(QStringLiteral("decision_too_large"),
                QStringLiteral("Agent decision exceeded 64 KiB."));
}

void AgentController::completeGeneration(bool cancelled,
                                         const QJsonObject& metrics)
{
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding) return;
    const auto promptTokens =
        metrics.value(QStringLiteral("promptTokens")).toInt();
    if (promptTokens > 0) activeRun_->lastPromptTokens = promptTokens;
    if (cancelled)
    {
        failRun(QStringLiteral("generation_cancelled"),
                QStringLiteral("Agent decision generation was cancelled."));
        return;
    }

    auto* task = currentTask();
    if (!task || !task->hasConversation())
    {
        failRun(
            QStringLiteral("task_state_invalid"),
            QStringLiteral("No active task can complete the model response."));
        return;
    }
    const auto verificationTaskId =
        task->kind() == AgentTask::Kind::Execution ? task->id() : QString{};
    const auto taskContext =
        task->kind() == AgentTask::Kind::Planning
            ? activeRun_->userRequest
            : activeRun_->ledger.unresolvedVerificationReason(
                  verificationTaskId);
    const auto directive =
        task->completeTaskGeneration(cancelled, toolEvidence_, taskContext);
    if (task->kind() == AgentTask::Kind::Planning &&
        activeRun_->planningTask.has_value())
        activeRun_->taskPlanFailures = activeRun_->planningTask->repairCount();
    applyTaskDirective(directive);
}

void AgentController::handleGenerationError(const QString& code,
                                            const QString& message)
{
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding) return;
    failRun(code.isEmpty() ? QStringLiteral("generation_failed") : code,
            message);
}

void AgentController::receiveToolResult(const agent::ToolResult& result)
{
    if (!hasActiveRun() || state_ != AgentRun::State::ExecutingTool) return;
    auto completedCall =
        toolRuntime_.completeCall(result, QDateTime::currentMSecsSinceEpoch());
    if (!completedCall.has_value()) return;
    const auto completedToolAction = std::move(completedCall->action);
    auto normalizedResult = std::move(completedCall->result);
    const auto completedOperationKind = completedCall->operationKind;
    const auto outcome = normalizedResult.outcome;
    const auto inProgress = outcome == agent::ToolOutcome::InProgress;
    if (!inProgress)
    {
        if (completedOperationKind == ToolOperationKind::ReadOnly &&
            isDiscoveryToolName(completedToolAction.toolName))
            ++activeRun_->consecutiveDiscoveryCalls;
        else
            activeRun_->consecutiveDiscoveryCalls = 0;
    }
    const auto evidenceSequence = static_cast<int>(toolEvidence_.size() + 1);
    toolEvidence_.append(AgentContextCompactor::toolEvidence(
        evidenceSequence, completedToolAction, normalizedResult));
    const auto* executionTask = currentExecutionTask();
    const auto verificationTaskId =
        executionTask ? executionTask->id() : QString{};
    activeRun_->ledger.recordToolResult(verificationTaskId, evidenceSequence,
                                        completedToolAction, normalizedResult,
                                        completedOperationKind);
    ++activeRun_->evidenceRevision;
    if (outcome == agent::ToolOutcome::Succeeded)
        ++activeRun_->successfulToolResults;
    activeRun_->stagnationRecoveries = 0;
    recordEvent(agent::EventType::ToolFinished,
                outcome == agent::ToolOutcome::Succeeded
                    ? QStringLiteral("Tool call completed.")
                : inProgress ? QStringLiteral("Tool call is still in progress.")
                             : normalizedResult.errorMessage,
                normalizedResult.serverId + QLatin1Char('.') +
                    normalizedResult.toolName,
                normalizedResult.result);
    auto* task = currentExecutionTask();
    if (!task || !task->hasConversation())
    {
        failRun(QStringLiteral("task_state_invalid"),
                QStringLiteral(
                    "No active execution task can receive the tool result."));
        return;
    }
    task->receiveToolResult(
        normalizedResult, evidenceSequence, activeRun_->ledger.snapshot(),
        activeRun_->ledger.unresolvedVerificationReason(task->id()));

    const auto uncertainDispatch = normalizedResult.sideEffectState ==
                                   agent::ToolSideEffectState::Uncertain;
    const auto remoteFailure = outcome == agent::ToolOutcome::TransportFailed ||
                               outcome == agent::ToolOutcome::ProtocolFailed ||
                               outcome == agent::ToolOutcome::ServerFailed ||
                               outcome == agent::ToolOutcome::Cancelled;
    if (remoteFailure && uncertainDispatch &&
        completedOperationKind != ToolOperationKind::ReadOnly)
    {
        requestVerification(
            QStringLiteral(
                "The tool request was dispatched, but its final outcome is "
                "unknown. Continuing requires recovery guidance or explicit "
                "acceptance; the same mutation will not be retried."),
            VerificationContinuation::ResumeDecision, verificationTaskId);
        return;
    }
    if (outcome == agent::ToolOutcome::Cancelled)
    {
        failRun(QStringLiteral("tool_cancelled"),
                normalizedResult.errorMessage.isEmpty()
                    ? QStringLiteral("The tool request was cancelled.")
                    : normalizedResult.errorMessage);
        return;
    }
    if (outcome == agent::ToolOutcome::TransportFailed &&
        (normalizedResult.sideEffectState ==
             agent::ToolSideEffectState::NotDispatched ||
         completedOperationKind == ToolOperationKind::ReadOnly) &&
        completedOperationKind == ToolOperationKind::ReadOnly &&
        activeRun_->readOnlyTransportRetries < 1)
    {
        ++activeRun_->readOnlyTransportRetries;
        const QJsonObject retryAction{
            {QStringLiteral("action"), QStringLiteral("call_tool")},
            {QStringLiteral("tool"), completedToolAction.toolName},
            {QStringLiteral("arguments"), completedToolAction.arguments}};
        decisionMessages().append(
            {chat::Role::Assistant,
             QString::fromUtf8(
                 QJsonDocument(retryAction).toJson(QJsonDocument::Compact))});
        executeTool(completedToolAction);
        return;
    }
    if (outcome == agent::ToolOutcome::Denied)
    {
        failRun(QStringLiteral("tool_denied"),
                normalizedResult.errorMessage.isEmpty()
                    ? QStringLiteral("The tool request was denied.")
                    : normalizedResult.errorMessage);
        return;
    }
    requestDecision();
}

bool AgentController::isTerminal(AgentRun::State state)
{
    return state == AgentRun::State::Completed ||
           state == AgentRun::State::Blocked ||
           state == AgentRun::State::Cancelled ||
           state == AgentRun::State::Failed;
}

ExecutionTask* AgentController::currentExecutionTask()
{
    if (!activeRun_) return nullptr;
    if (activeRun_->directTask.has_value()) return &*activeRun_->directTask;
    if (activeRun_->executionTasks.empty()) return nullptr;
    const auto index = activeRun_->currentExecutionTaskIndex;
    if (index >= 0 &&
        index < static_cast<int>(activeRun_->executionTasks.size()))
        return &activeRun_->executionTasks.at(static_cast<std::size_t>(index));
    return nullptr;
}

AgentTask* AgentController::currentTask()
{
    if (!activeRun_) return nullptr;
    if (activeRun_->taskPlanRequired && activeRun_->planningTask.has_value())
        return &*activeRun_->planningTask;
    if (activeRun_->summaryTask.has_value()) return &*activeRun_->summaryTask;
    return currentExecutionTask();
}

const ExecutionTask* AgentController::currentExecutionTask() const
{
    if (!activeRun_) return nullptr;
    if (activeRun_->directTask.has_value()) return &*activeRun_->directTask;
    if (activeRun_->executionTasks.empty()) return nullptr;
    const auto index = activeRun_->currentExecutionTaskIndex;
    if (index >= 0 &&
        index < static_cast<int>(activeRun_->executionTasks.size()))
        return &activeRun_->executionTasks.at(static_cast<std::size_t>(index));
    return nullptr;
}

const AgentTask* AgentController::currentTask() const
{
    if (!activeRun_) return nullptr;
    if (activeRun_->taskPlanRequired && activeRun_->planningTask.has_value())
        return &*activeRun_->planningTask;
    if (activeRun_->summaryTask.has_value()) return &*activeRun_->summaryTask;
    return currentExecutionTask();
}

QList<chat::Message>& AgentController::decisionMessages()
{
    if (auto* task = currentTask(); task && task->hasConversation())
        return task->messages();
    return activeRun_->inferenceMessages;
}

qsizetype& AgentController::decisionRequestMessageIndex()
{
    if (auto* task = currentTask(); task && task->hasConversation())
        return task->requestMessageIndex();
    return activeRun_->requestMessageIndex;
}

void AgentController::refreshExecutionTaskSnapshots()
{
    if (!activeRun_ || activeRun_->executionTasks.empty()) return;
    QJsonArray snapshots;
    for (const auto& task : activeRun_->executionTasks)
        snapshots.append(task.completionSnapshot());
    activeRun_->completionSteps = std::move(snapshots);
}

void AgentController::activateExecutionTask(int index, int evidenceStart)
{
    if (!activeRun_ || index < 0 ||
        index >= static_cast<int>(activeRun_->executionTasks.size()))
        return;

    QJsonArray completedSteps;
    for (auto completedIndex = 0; completedIndex < index; ++completedIndex)
    {
        const auto& completed = activeRun_->executionTasks.at(
            static_cast<std::size_t>(completedIndex));
        if (completed.status() == ExecutionTask::Status::Completed)
            completedSteps.append(completed.completionSnapshot());
    }

    auto messages = activeRun_->inferenceMessages;
    auto& task = activeRun_->executionTasks.at(static_cast<std::size_t>(index));
    task.activate(std::move(messages), completedSteps, toolEvidence_,
                  evidenceStart, activeRun_->userRequest);
    recordTaskStarted(task, index + 1,
                      static_cast<int>(activeRun_->executionTasks.size()));

    // Recovery windows belong to one task worker, not the whole plan.
    activeRun_->consecutiveValidationFailures = 0;
    activeRun_->stagnationRecoveries = 0;
    activeRun_->orderedPlanRepairs = 0;
    activeRun_->readOnlyTransportRetries = 0;
    activeRun_->consecutiveDiscoveryCalls = 0;
    toolRuntime_.resetCallHistory();
}

void AgentController::activateSummaryTask()
{
    if (!activeRun_) return;
    activeRun_->summaryTask.emplace();
    activeRun_->summaryTask->activate(
        activeRun_->inferenceMessages, activeRun_->userRequest,
        activeRun_->completionSteps, toolEvidence_);
    recordTaskStarted(*activeRun_->summaryTask);
}

void AgentController::requestDecision(const QString& operation)
{
    if (!activeRun_) return;
    auto* task = currentTask();
    if (!task || !task->hasConversation())
    {
        failRun(QStringLiteral("task_state_invalid"),
                QStringLiteral("No active task can request a model response."));
        return;
    }
    activeRun_->decisionOperation = operation.trimmed();
    ++activeRun_->decisionCount;
    compactContextIfNeeded();
    setState(AgentRun::State::Deciding);
    recordEvent(agent::EventType::DecisionStarted,
                QStringLiteral("Generating the next agent decision."));
    activeRun_->lastSubmittedCharacters = messageCharacters(decisionMessages());
    const auto outputTokens =
        qMax(minimumDecisionTokens, preset_.maxOutputTokens);
    task->requestDecision(dependencies_.generate, preset_, outputTokens);
    emitProgressChanged();
}

void AgentController::compactContextIfNeeded()
{
    if (!activeRun_) return;
    auto estimatedPromptTokens = activeRun_->lastPromptTokens;
    const auto currentCharacters = messageCharacters(decisionMessages());
    if (estimatedPromptTokens > 0 && activeRun_->lastSubmittedCharacters > 0 &&
        currentCharacters > activeRun_->lastSubmittedCharacters)
    {
        estimatedPromptTokens = static_cast<int>(
            (static_cast<qint64>(estimatedPromptTokens) * currentCharacters +
             activeRun_->lastSubmittedCharacters - 1) /
            activeRun_->lastSubmittedCharacters);
    }
    if (!AgentContextCompactor::shouldCompact(estimatedPromptTokens, preset_))
        return;
    auto visibleSteps = activeRun_->completionSteps;
    auto compactionRequest = activeRun_->userRequest;
    if (!activeRun_->executionTasks.empty())
    {
        visibleSteps = QJsonArray{};
        const auto lastVisibleIndex =
            std::min(activeRun_->currentExecutionTaskIndex,
                     static_cast<int>(activeRun_->executionTasks.size()) - 1);
        for (auto index = 0; index <= lastVisibleIndex; ++index)
            visibleSteps.append(
                activeRun_->executionTasks.at(static_cast<std::size_t>(index))
                    .completionSnapshot());

        if (activeRun_->currentExecutionTaskIndex >= 0 &&
            activeRun_->currentExecutionTaskIndex <
                static_cast<int>(activeRun_->executionTasks.size()))
        {
            const auto current = currentExecutionTask()->specification();
            compactionRequest =
                QStringLiteral(
                    "Continue only the manager-assigned current task: %1")
                    .arg(QString::fromUtf8(
                        QJsonDocument(current).toJson(QJsonDocument::Compact)));
        }
        else
        {
            compactionRequest = QStringLiteral(
                "All manager-assigned tasks are verified. Prepare only the "
                "final user-facing answer.");
        }
    }
    const QJsonObject completionState{
        {QStringLiteral("steps"), visibleSteps},
        {QStringLiteral("ordered"), !activeRun_->executionTasks.empty()},
        {QStringLiteral("currentStepIndex"),
         activeRun_->currentExecutionTaskIndex},
        {QStringLiteral("evidenceRevision"), activeRun_->evidenceRevision},
        {QStringLiteral("currentStepEvidenceStart"),
         currentExecutionTask() ? currentExecutionTask()->evidenceStart() : 1},
        {QStringLiteral("currentStepEvidenceEnd"),
         currentExecutionTask() ? currentExecutionTask()->evidenceEnd() : 0}};
    const auto result = AgentContextCompactor::compact(
        decisionMessages(), decisionRequestMessageIndex(), compactionRequest,
        toolEvidence_, completionState, activeRun_->ledger.snapshot(), preset_);
    if (!result.compacted) return;
    ++activeRun_->contextCompactions;
    qInfo().noquote()
        << QStringLiteral(
               "Agent context compacted: run=%1 promptTokens=%2 "
               "estimatedPromptTokens=%3 messages=%4->%5 evidence=%6 "
               "compactions=%7")
               .arg(activeRun_->id)
               .arg(activeRun_->lastPromptTokens)
               .arg(estimatedPromptTokens)
               .arg(result.messagesBefore)
               .arg(result.messagesAfter)
               .arg(toolEvidence_.size())
               .arg(activeRun_->contextCompactions);
    activeRun_->lastPromptTokens = 0;
}

void AgentController::handleToolAction(const agent::Action& action,
                                       const QByteArray& rawAction,
                                       bool recordAction)
{
    if (!activeRun_) return;
    if (action.type != agent::ActionType::CallTool)
    {
        failRun(QStringLiteral("task_state_invalid"),
                QStringLiteral(
                    "A non-tool task directive reached the tool dispatcher."));
        return;
    }

    ++activeRun_->toolActionAttempts;
    const auto descriptor = toolRuntime_.inspect(action.toolName);
    if (!descriptor.has_value())
    {
        retryTaskAction(
            rawAction,
            QStringLiteral("Tool is not available: %1").arg(action.toolName),
            recordAction);
        return;
    }
    const auto candidateOperationKind = descriptor->operationKind;
    const auto recordDuplicateToolAction =
        [this, &action, candidateOperationKind]
    {
        if (!activeRun_) return;
        ++activeRun_->duplicateToolActions;
        if (candidateOperationKind == ToolOperationKind::Mutation)
            ++activeRun_->duplicateMutationActions;
        if (candidateOperationKind == ToolOperationKind::ReadOnly &&
            isDiscoveryToolName(action.toolName))
            ++activeRun_->redundantDiscoveryCalls;
    };
    if (!activeRun_->executionTasks.empty())
    {
        if (const auto* task = currentExecutionTask())
        {
            const auto taskError = task->validateToolAction(
                action, candidateOperationKind == ToolOperationKind::ReadOnly);
            if (!taskError.isEmpty())
            {
                retryTaskAction(rawAction, taskError, recordAction);
                return;
            }
            activeRun_->orderedPlanRepairs = 0;
        }
        else
        {
            retryNoProgressAction(
                rawAction,
                QStringLiteral(
                    "All task-plan tasks are already verified. Return final "
                    "instead of executing another tool."),
                recordAction);
            return;
        }
    }

    const auto callGuard = toolRuntime_.guardCall(action);
    if (!callGuard.allowed())
    {
        recordDuplicateToolAction();
        retryNoProgressAction(rawAction, callGuard.errorMessage, recordAction);
        return;
    }

    ++activeRun_->toolValidationAttempts;
    const auto validation = toolRuntime_.validate(action);
    if (!validation.valid)
    {
        ++activeRun_->toolValidationFailures;
        auto issue = validation.issue;
        if (issue.toolName.isEmpty()) issue.toolName = action.toolName;
        if (issue.instancePath.isEmpty())
            issue.instancePath = QStringLiteral("arguments");
        if (issue.schemaPath.isEmpty()) issue.schemaPath = QStringLiteral("#");
        if (issue.keyword.isEmpty())
            issue.keyword = QStringLiteral("validation");
        if (issue.message.isEmpty())
            issue.message =
                QStringLiteral("Tool arguments failed local validation.");
        retryInvalidToolAction(rawAction, issue, descriptor->definition,
                               action.arguments, recordAction);
        return;
    }
    activeRun_->consecutiveValidationFailures = 0;
    const auto isBreadthDiscovery =
        candidateOperationKind == ToolOperationKind::ReadOnly &&
        isDiscoveryToolName(action.toolName);
    if (isBreadthDiscovery &&
        activeRun_->consecutiveDiscoveryCalls >=
            maximumConsecutiveDiscoveryCalls &&
        !explicitlyRequestsExhaustiveDiscovery(activeRun_->userRequest) &&
        !callGuard.statusPoll)
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "The run already made %1 consecutive read-only discovery "
                "calls without executing another requested operation. Do "
                "not broaden the inventory. Use existing evidence to return "
                "final, or call only a non-discovery tool required by an "
                "explicit unfinished outcome.")
                .arg(maximumConsecutiveDiscoveryCalls),
            recordAction);
        return;
    }
    if (recordAction)
        decisionMessages().append(
            {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    dispatchTool(action);
}

void AgentController::applyTaskDirective(const AgentTask::Directive& directive)
{
    if (!activeRun_) return;
    switch (directive.type)
    {
        case AgentTask::Directive::Type::Generate:
        {
            const auto operation =
                directive.activity.isEmpty() && currentTask()
                    ? currentTask()->runtimeSnapshot().activity
                    : directive.activity;
            recordEvent(agent::EventType::RecoveryStarted,
                        operation + QLatin1Char('.'));
            requestDecision(operation);
            return;
        }
        case AgentTask::Directive::Type::TasksCreated:
            acceptTaskPlan(directive.tasks);
            return;
        case AgentTask::Directive::Type::CallTool:
            handleToolAction(directive.toolAction, directive.rawAction);
            return;
        case AgentTask::Directive::Type::Completed:
            if (currentTask() &&
                currentTask()->kind() == AgentTask::Kind::Summary)
            {
                completeRun(directive.content);
                return;
            }
            if (activeRun_->executionTasks.empty())
            {
                completeRun(directive.content);
                return;
            }
            if (auto* task = currentExecutionTask())
            {
                recordEvent(agent::EventType::TaskStepUpdated,
                            QStringLiteral("Task plan step verified."), {},
                            {{QStringLiteral("stepId"), task->id()},
                             {QStringLiteral("evidenceSequence"),
                              directive.evidenceEnd}});
                ++activeRun_->currentExecutionTaskIndex;
                refreshExecutionTaskSnapshots();
                if (activeRun_->currentExecutionTaskIndex <
                    static_cast<int>(activeRun_->executionTasks.size()))
                {
                    const auto evidenceStart =
                        directive.evidenceEnd > 0
                            ? directive.evidenceEnd + 1
                            : static_cast<int>(toolEvidence_.size() + 1);
                    activateExecutionTask(activeRun_->currentExecutionTaskIndex,
                                          evidenceStart);
                }
                else
                {
                    const auto reason =
                        activeRun_->ledger.unresolvedVerificationReason();
                    if (!reason.isEmpty())
                    {
                        requestVerification(
                            reason, VerificationContinuation::StartSummary);
                        return;
                    }
                    activateSummaryTask();
                }
                requestDecision();
            }
            return;
        case AgentTask::Directive::Type::Blocked:
            blockRun(directive.code, directive.content);
            return;
        case AgentTask::Directive::Type::VerificationRequired:
            requestVerification(directive.detail,
                                VerificationContinuation::RepeatCurrentDecision,
                                currentExecutionTask()
                                    ? currentExecutionTask()->id()
                                    : QString{});
            return;
        case AgentTask::Directive::Type::Failed:
            failRun(directive.code, directive.detail);
            return;
        case AgentTask::Directive::Type::Continue:
        {
            const auto operation =
                directive.activity.isEmpty() && currentTask()
                    ? currentTask()->runtimeSnapshot().activity
                    : directive.activity;
            recordEvent(agent::EventType::TaskStepUpdated,
                        QStringLiteral("Current task remains active."), {},
                        {{QStringLiteral("stepId"),
                          currentExecutionTask() ? currentExecutionTask()->id()
                                                 : QString{}}});
            requestDecision(operation);
            return;
        }
    }
}

void AgentController::dispatchTool(const agent::Action& action)
{
    if (!activeRun_) return;
    const auto decision = toolRuntime_.authorize(action);
    if (decision == infrastructure::mcp::ToolDecision::Deny)
    {
        failRun(QStringLiteral("tool_denied"),
                QStringLiteral("Local policy denied the tool call."));
        return;
    }
    if (decision == infrastructure::mcp::ToolDecision::RequireApproval)
    {
        if (auto* task = currentExecutionTask()) task->awaitApproval();
        setState(AgentRun::State::WaitingForApproval);
        recordEvent(agent::EventType::ApprovalRequested,
                    QStringLiteral("Tool call requires approval."),
                    action.toolName, action.arguments);
        emit approvalRequested(activeRun_->id, action.toolName,
                               action.arguments);
        return;
    }
    executeTool(action);
}

QJsonObject AgentController::verificationContext(const QString& taskId) const
{
    if (!activeRun_) return {};
    auto context = activeRun_->ledger.unresolvedVerificationContext(taskId);
    const auto contextTaskId =
        context.value(QStringLiteral("taskId")).toString(taskId);
    context.insert(QStringLiteral("taskId"), contextTaskId);
    if (activeRun_->directTask.has_value())
    {
        context.insert(QStringLiteral("taskDescription"),
                       activeRun_->directTask->description());
        context.insert(QStringLiteral("stepNumber"), 1);
        context.insert(QStringLiteral("stepCount"), 1);
    }
    else
    {
        for (std::size_t index = 0; index < activeRun_->executionTasks.size();
             ++index)
        {
            const auto& task = activeRun_->executionTasks.at(index);
            if (task.id() != contextTaskId) continue;
            context.insert(QStringLiteral("taskDescription"),
                           task.description());
            context.insert(QStringLiteral("stepNumber"),
                           static_cast<int>(index + 1));
            context.insert(QStringLiteral("stepCount"),
                           static_cast<int>(activeRun_->executionTasks.size()));
            break;
        }
    }
    return context;
}

void AgentController::requestVerification(const QString& reason,
                                          VerificationContinuation continuation,
                                          const QString& taskId)
{
    if (!activeRun_) return;
    verificationContinuation_ = continuation;
    verificationTaskId_ = taskId;
    verificationReason_ = reason.trimmed();
    const auto context = verificationContext(verificationTaskId_);
    if (auto* task = currentExecutionTask()) task->awaitVerification();
    setState(AgentRun::State::WaitingForVerification);
    recordEvent(
        agent::EventType::VerificationRequested,
        QStringLiteral("Agent run is waiting for user-supplied evidence or "
                       "confirmation."),
        {},
        {{QStringLiteral("reason"), verificationReason_},
         {QStringLiteral("taskId"), verificationTaskId_}});
    emit verificationRequested(activeRun_->id, verificationReason_, context);
}

void AgentController::clearPendingVerification()
{
    verificationContinuation_.reset();
    verificationTaskId_.clear();
    verificationReason_.clear();
}

void AgentController::acceptTaskPlan(const QJsonArray& steps)
{
    if (!activeRun_) return;
    activeRun_->completionSteps = steps;
    activeRun_->currentExecutionTaskIndex = 0;
    activeRun_->taskPlanRequired = false;
    qInfo().noquote() << QStringLiteral(
                             "Agent task plan accepted: run=%1 steps=%2")
                             .arg(activeRun_->id)
                             .arg(activeRun_->completionSteps.size());
    QJsonArray sourceMap;
    for (const auto& value : steps)
    {
        const auto step = value.toObject();
        sourceMap.append(QJsonObject{
            {QStringLiteral("stepId"), step.value(QStringLiteral("id"))},
            {QStringLiteral("sourceIds"),
             step.value(QStringLiteral("source_ids"))}});
    }
    recordEvent(
        agent::EventType::TaskPlanAccepted,
        QStringLiteral("Task plan accepted."), {},
        {{QStringLiteral("stepCount"), activeRun_->completionSteps.size()},
         {QStringLiteral("sourceMap"), sourceMap}});
    activeRun_->executionTasks.clear();
    activeRun_->executionTasks.reserve(static_cast<std::size_t>(steps.size()));
    for (qsizetype index = 0; index < steps.size(); ++index)
        activeRun_->executionTasks.emplace_back(steps.at(index).toObject());

    // Each ExecutionTask gets a fresh user turn containing its assignment,
    // Host-bound source references, and the original request.
    if (activeRun_->requestMessageIndex >= 0 &&
        activeRun_->requestMessageIndex < activeRun_->inferenceMessages.size())
        activeRun_->inferenceMessages.resize(activeRun_->requestMessageIndex);
    refreshExecutionTaskSnapshots();
    activateExecutionTask(0, static_cast<int>(toolEvidence_.size() + 1));
    requestDecision();
}

void AgentController::executeTool(const agent::Action& action)
{
    if (!activeRun_) return;
    if (auto* task = currentExecutionTask()) task->awaitTool();
    setState(AgentRun::State::ExecutingTool);
    const auto dispatch =
        toolRuntime_.dispatch(action, QDateTime::currentMSecsSinceEpoch());
    if (dispatch.status == AgentToolRuntime::DispatchResult::Status::Delayed)
    {
        pollTimer_->start(dispatch.delayMilliseconds);
        return;
    }
    if (dispatch.status == AgentToolRuntime::DispatchResult::Status::Failed)
    {
        failRun(QStringLiteral("tool_call_failed"),
                QStringLiteral("Tool call could not be started."));
        return;
    }
    if (dispatch.statusPoll) ++activeRun_->pollRequests;
    ++activeRun_->executedToolCalls;
    recordEvent(agent::EventType::ToolStarted,
                QStringLiteral("Tool call started."), action.toolName,
                action.arguments);
}

void AgentController::executePendingPoll()
{
    if (!hasActiveRun()) return;
    const auto action = toolRuntime_.takePendingPoll();
    if (!action.has_value()) return;
    executeTool(*action);
}

void AgentController::retryInvalidToolAction(
    const QByteArray& rawAction, const agent::ToolValidationIssue& issue,
    const agent::ToolDefinition& tool, const QJsonObject& arguments,
    bool recordAction)
{
    if (!activeRun_) return;
    if (activeRun_->consecutiveValidationFailures >= 1)
    {
        failRun(QStringLiteral("invalid_tool_arguments"), issue.message);
        return;
    }
    ++activeRun_->validationRepairs;
    ++activeRun_->consecutiveValidationFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent tool validation correction: run=%1 repairs=%2 "
               "reason=%3 tool=%4 path=%5 keyword=%6 action=%7")
               .arg(activeRun_->id)
               .arg(activeRun_->validationRepairs)
               .arg(singleLine(issue.message), tool.qualifiedName,
                    issue.instancePath, issue.keyword,
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    auto* task = currentTask();
    if (!task || !task->appendCorrectionTurn(
                     rawAction,
                     AgentPromptBuilder::toolValidationCorrectionMessage(
                         tool, arguments, issue),
                     recordAction))
    {
        failRun(
            QStringLiteral("task_conversation_invalid"),
            QStringLiteral("Could not append a valid tool correction turn."));
        return;
    }
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing invalid tool arguments."),
                tool.qualifiedName);
    requestDecision(
        QStringLiteral("Correcting arguments for %1").arg(tool.qualifiedName));
}

void AgentController::retryNoProgressAction(const QByteArray& rawAction,
                                            const QString& errorMessage,
                                            bool recordAction)
{
    if (!activeRun_) return;
    if (activeRun_->stagnationRecoveries >= 1)
    {
        failRun(QStringLiteral("agent_stalled"), errorMessage);
        return;
    }
    ++activeRun_->stagnationRecoveries;
    qWarning().noquote()
        << QStringLiteral(
               "Agent stagnation recovery: run=%1 attempt=%2 reason=%3 "
               "action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->stagnationRecoveries)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    auto* task = currentTask();
    if (!task ||
        !task->appendCorrectionTurn(
            rawAction, AgentPromptBuilder::noProgressMessage(errorMessage),
            recordAction))
    {
        failRun(QStringLiteral("task_conversation_invalid"),
                QStringLiteral("Could not append a valid recovery turn."));
        return;
    }
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Recovering from a repeated action."));
    requestDecision(QStringLiteral("Choosing a different action"));
}

void AgentController::retryTaskAction(const QByteArray& rawAction,
                                      const QString& errorMessage,
                                      bool recordAction)
{
    if (!activeRun_) return;
    auto* task = currentExecutionTask();
    if (!task || !task->repairAction(rawAction, errorMessage, recordAction))
    {
        failRun(QStringLiteral("invalid_agent_action"), errorMessage);
        return;
    }
    ++activeRun_->orderedPlanRepairs;
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing an ordered task-plan action."));
    requestDecision(QStringLiteral("Correcting the current task action"));
}

void AgentController::setState(AgentRun::State state)
{
    if (state_ == state) return;
    state_ = state;
    if (activeRun_) activeRun_->state = state;
    emit stateChanged(state_);
    emitProgressChanged();
}

void AgentController::recordEvent(agent::EventType type, const QString& message,
                                  const QString& toolName,
                                  const QJsonObject& data)
{
    if (!activeRun_) return;
    agent::Event event{activeRun_->id, type, message,
                       toolName,       data, QDateTime::currentDateTimeUtc()};
    activeRun_->events.append(event);
    auto logMessage = QStringLiteral("Agent event: run=%1 type=%2")
                          .arg(activeRun_->id, agent::eventTypeName(type));
    if (!toolName.isEmpty())
        logMessage += QStringLiteral(" tool=%1").arg(toolName);
    if (!message.isEmpty())
        logMessage += QStringLiteral(" message=%1").arg(singleLine(message));
    if (!data.isEmpty())
        logMessage += QStringLiteral(" data=%1").arg(eventDataSummary(data));
    qInfo().noquote() << logMessage;
    emit eventRecorded(event);
    emitProgressChanged();
}

void AgentController::recordTaskStarted(const AgentTask& task, int ordinal,
                                        int total)
{
    QJsonObject data{{QStringLiteral("taskKind"), task.kindName()},
                     {QStringLiteral("taskId"), task.id()},
                     {QStringLiteral("description"), task.description()}};
    if (ordinal > 0) data.insert(QStringLiteral("taskIndex"), ordinal);
    if (total > 0) data.insert(QStringLiteral("taskCount"), total);
    recordEvent(agent::EventType::TaskStarted,
                QStringLiteral("%1 task started.").arg(task.kindName()), {},
                data);
}

void AgentController::emitProgressChanged()
{
    if (!activeRun_) return;
    emit progressChanged(progressSnapshot());
}

void AgentController::completeRun(const QString& content)
{
    if (!activeRun_) return;
    clearPendingVerification();
    activeRun_->finishCode.clear();
    activeRun_->finishMessage.clear();
    setState(AgentRun::State::GeneratingAnswer);
    recordEvent(agent::EventType::AnswerStarted,
                QStringLiteral("Preparing final answer."));
    emit finalAnswerReady(activeRun_->id, content);
    conversationMessages_.append({chat::Role::User, activeRun_->userRequest});
    conversationMessages_.append({chat::Role::Assistant, content});
    setState(AgentRun::State::Completed);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    toolRuntime_.clearTransportState();
    recordEvent(agent::EventType::Completed,
                QStringLiteral("Agent run completed."));
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, {}, {});
}

void AgentController::blockRun(const QString& reason, const QString& content)
{
    if (!activeRun_) return;
    clearPendingVerification();
    const auto code = QStringLiteral("blocked_%1").arg(reason);
    activeRun_->finishCode = code;
    activeRun_->finishMessage = content;
    QString blockedStepId;
    const auto currentIndex = activeRun_->currentExecutionTaskIndex;
    if (currentIndex >= 0 &&
        currentIndex < static_cast<int>(activeRun_->executionTasks.size()))
    {
        auto& task = activeRun_->executionTasks.at(
            static_cast<std::size_t>(currentIndex));
        task.markBlocked();
        blockedStepId = task.id();
        refreshExecutionTaskSnapshots();
    }
    setState(AgentRun::State::Blocked);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    toolRuntime_.clearTransportState();
    if (!blockedStepId.isEmpty())
        recordEvent(agent::EventType::TaskStepUpdated,
                    QStringLiteral("Task plan step blocked."), {},
                    {{QStringLiteral("stepId"), blockedStepId},
                     {QStringLiteral("reason"), reason}});
    emit finalAnswerReady(activeRun_->id, content);
    conversationMessages_.append({chat::Role::User, activeRun_->userRequest});
    conversationMessages_.append({chat::Role::Assistant, content});
    recordEvent(agent::EventType::Blocked, QStringLiteral("Agent run blocked."),
                {}, {{QStringLiteral("reason"), reason}});
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, code, content);
}

void AgentController::failRun(const QString& code, const QString& message)
{
    if (!activeRun_ || isTerminal(state_)) return;
    clearPendingVerification();
    const auto previousState = state_;
    activeRun_->finishCode = code;
    activeRun_->finishMessage = message;
    if (auto* task = currentTask())
    {
        task->fail();
        if (activeRun_->currentExecutionTaskIndex >= 0 &&
            activeRun_->currentExecutionTaskIndex <
                static_cast<int>(activeRun_->executionTasks.size()))
            refreshExecutionTaskSnapshots();
    }
    setState(AgentRun::State::Failed);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    toolRuntime_.clearPendingApproval();
    if (previousState == AgentRun::State::Deciding &&
        dependencies_.cancelGeneration)
        dependencies_.cancelGeneration();
    if (previousState == AgentRun::State::ExecutingTool)
        toolRuntime_.cancelActiveCall();
    else
        toolRuntime_.clearTransportState();
    recordEvent(agent::EventType::Failed, message);
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, code, message);
}
}  // namespace qtllm::application
