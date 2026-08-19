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
constexpr auto minimumPollIntervalMilliseconds = 1'000;
constexpr auto maximumCompletionReviewsPerEvidenceRevision = 2;
constexpr auto maximumConsecutiveDiscoveryCalls = 4;
constexpr qsizetype maximumDetectedCycleLength = 4;
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

QJsonValue canonicalJsonValue(const QJsonValue& value)
{
    if (value.isString())
    {
        auto text = value.toString();
        if (text.size() >= 3 && text.at(1) == QLatin1Char(':') &&
            (text.at(2) == QLatin1Char('/') || text.at(2) == QLatin1Char('\\')))
            text.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return text;
    }
    if (value.isArray())
    {
        QJsonArray result;
        for (const auto& item : value.toArray())
            result.append(canonicalJsonValue(item));
        return result;
    }
    if (value.isObject())
    {
        QJsonObject result;
        const auto object = value.toObject();
        for (const auto& key : object.keys())
            result.insert(key, canonicalJsonValue(object.value(key)));
        return result;
    }
    return value;
}

QString toolCallSignature(const agent::Action& action)
{
    const auto arguments = canonicalJsonValue(action.arguments).toObject();
    return action.toolName + QLatin1Char('\n') +
           QString::fromUtf8(
               QJsonDocument(arguments).toJson(QJsonDocument::Compact));
}

QString repeatedCompletedCallError(const QStringList& history,
                                   const QString& candidate)
{
    auto sequence = history;
    sequence.append(candidate);
    for (qsizetype cycleLength = 1; cycleLength <= maximumDetectedCycleLength &&
                                    sequence.size() >= cycleLength * 2;
         ++cycleLength)
    {
        const auto cycleStart = sequence.size() - cycleLength * 2;
        auto repeats = true;
        for (qsizetype offset = 0; offset < cycleLength; ++offset)
        {
            if (sequence.at(cycleStart + offset) !=
                sequence.at(cycleStart + cycleLength + offset))
            {
                repeats = false;
                break;
            }
        }
        if (!repeats) continue;
        if (cycleLength == 1)
            return QStringLiteral(
                "The identical tool call already completed successfully. "
                "Do not execute it again. Continue with a different "
                "unfinished step, or return final if all requested work is "
                "complete.");
        return QStringLiteral(
            "This tool call would continue a repeated cycle of completed "
            "calls. Do not toggle resources open and closed or repeat "
            "completed work. Continue with a different unfinished step, or "
            "return final if all requested work is complete.");
    }
    return {};
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

QString unfinishedEvidenceReason(const QList<QJsonObject>& toolEvidence,
                                 const AgentLedger& ledger)
{
    if (!toolEvidence.isEmpty())
    {
        const auto& latest = toolEvidence.constLast();
        if (latest.value(QStringLiteral("outcome")).toString() ==
            QLatin1String("error"))
            return QStringLiteral(
                "The most recent tool call failed, so the requested operation "
                "does not yet have successful evidence.");
        if (!latest.value(QStringLiteral("terminal")).toBool())
            return QStringLiteral(
                "The most recent tool result is still running or pending and "
                "is not terminal evidence.");
    }
    return ledger.unresolvedVerificationReason();
}

ToolOperationKind operationKind(
    const agent::ToolDefinition& tool,
    const AgentController::ToolRiskHandler& toolRisk)
{
    const auto readOnlyHint =
        tool.annotations.value(QStringLiteral("readOnlyHint"));
    if (readOnlyHint.isBool())
        return readOnlyHint.toBool() ? ToolOperationKind::ReadOnly
                                     : ToolOperationKind::Mutation;
    if (tool.annotations.value(QStringLiteral("destructiveHint")).toBool())
        return ToolOperationKind::Mutation;
    if (!toolRisk) return ToolOperationKind::Unknown;
    switch (toolRisk(tool.qualifiedName))
    {
        case infrastructure::mcp::ToolRisk::ReadOnly:
            return ToolOperationKind::ReadOnly;
        case infrastructure::mcp::ToolRisk::CreatesData:
        case infrastructure::mcp::ToolRisk::ModifiesData:
        case infrastructure::mcp::ToolRisk::Destructive:
            return ToolOperationKind::Mutation;
    }
    return ToolOperationKind::Unknown;
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
        !dependencies_.callTool || !dependencies_.validateTool ||
        !dependencies_.toolPolicy)
        return false;

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
    availableTools_ = tools;
    pendingApproval_.reset();
    activeToolAction_.reset();
    pendingPollAction_.reset();
    decisionBytes_.clear();
    activeToolCallSignature_.clear();
    lastFailedToolCallSignature_.clear();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    completedToolCallHistory_.clear();
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
    const auto toolRequestId = activeRun_->toolRequestId;
    activeRun_->finishCode = QStringLiteral("cancelled");
    activeRun_->finishMessage = QStringLiteral("Agent run cancelled.");
    if (auto* task = currentTask())
    {
        task->cancel();
        if (activeRun_->orderedTaskPlan) refreshExecutionTaskSnapshots();
    }
    setState(AgentRun::State::Cancelled);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    pendingApproval_.reset();
    activeToolAction_.reset();
    pendingPollAction_.reset();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    activeRun_->toolRequestId.clear();
    if (previousState == AgentRun::State::Deciding &&
        dependencies_.cancelGeneration)
        dependencies_.cancelGeneration();
    if (previousState == AgentRun::State::ExecutingTool &&
        !toolRequestId.isEmpty() && dependencies_.cancelTool)
        dependencies_.cancelTool(toolRequestId);
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
        !pendingApproval_.has_value())
        return;
    const auto action = *pendingApproval_;
    pendingApproval_.reset();
    if (!approved)
    {
        failRun(QStringLiteral("approval_denied"),
                QStringLiteral("The requested tool call was rejected."));
        return;
    }
    executeTool(action);
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
    auto assignedCurrentStep = false;
    for (qsizetype index = 0; index < run.completionSteps.size(); ++index)
    {
        const auto object = run.completionSteps.at(index).toObject();
        AgentProgressStep step;
        step.id = object.value(QStringLiteral("id")).toString();
        step.description =
            object.value(QStringLiteral("description")).toString();
        if (run.orderedTaskPlan &&
            index < static_cast<qsizetype>(run.executionTasks.size()))
        {
            const auto taskSnapshot =
                run.executionTasks.at(static_cast<std::size_t>(index))
                    .runtimeSnapshot();
            step.activity = taskSnapshot.activity;
            step.elapsedMilliseconds = taskSnapshot.elapsedMilliseconds;
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
        else if (!terminal && ((run.orderedTaskPlan &&
                                index == run.currentExecutionTaskIndex) ||
                               (!run.orderedTaskPlan && !assignedCurrentStep)))
        {
            step.status = AgentProgressStepStatus::Current;
            snapshot.currentStepId = step.id;
            assignedCurrentStep = true;
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
            if (run.orderedTaskPlan &&
                run.currentExecutionTaskIndex >=
                    static_cast<int>(run.executionTasks.size()))
                snapshot.operation =
                    QStringLiteral("Preparing the final answer");
            else if (run.taskPlanRequired && run.planningTask.has_value())
                snapshot.operation =
                    run.planningTask->runtimeSnapshot().activity;
            else if (const auto* task = currentExecutionTask();
                     task && task->awaitingReview())
                snapshot.operation =
                    QStringLiteral("Reviewing the current task result");
            else if (task && task->hasPendingToolCallReview())
                snapshot.operation =
                    QStringLiteral("Checking the current task boundary");
            else
                snapshot.operation = QStringLiteral("Working on current task");
            snapshot.waitingReason =
                QStringLiteral("Waiting for the model response");
            break;
        case AgentRun::State::WaitingForApproval:
            snapshot.operation = toolOperation(pendingApproval_);
            snapshot.waitingReason =
                QStringLiteral("Waiting for your approval");
            break;
        case AgentRun::State::ExecutingTool:
            snapshot.operation = toolOperation(activeToolAction_);
            if (snapshot.operation.isEmpty())
                snapshot.operation = toolOperation(pendingPollAction_);
            snapshot.waitingReason =
                pendingPollAction_.has_value()
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
    if (auto* task = currentTask(); task && task->hasConversation())
    {
        if (!task->receiveToken(bytes, maximumDecisionBytes))
            failRun(QStringLiteral("decision_too_large"),
                    QStringLiteral("Agent decision exceeded 64 KiB."));
        return;
    }
    decisionBytes_ += bytes;
    if (decisionBytes_.size() > maximumDecisionBytes)
    {
        failRun(QStringLiteral("decision_too_large"),
                QStringLiteral("Agent decision exceeded 64 KiB."));
    }
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

    if (auto* task = currentTask(); task && task->hasConversation())
    {
        const auto directive = task->completeTaskGeneration(
            cancelled, toolEvidence_,
            activeRun_->ledger.unresolvedVerificationReason());
        if (task->kind() == AgentTask::Kind::Planning &&
            activeRun_->planningTask.has_value())
            activeRun_->taskPlanFailures =
                activeRun_->planningTask->repairCount();
        applyTaskDirective(directive);
        return;
    }

    agent::Action action;
    QByteArray rawAction;
    QString errorMessage;
    rawAction = decisionBytes_;
    if (!agent::parseAction(rawAction, action, errorMessage))
    {
        retryInvalidAction(rawAction, errorMessage);
        return;
    }
    handleAction(action, rawAction);
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
    if (!hasActiveRun() || state_ != AgentRun::State::ExecutingTool ||
        result.requestId != activeRun_->toolRequestId)
        return;
    auto normalizedResult = result;
    normalizedResult.outcome = normalizedToolOutcome(result);
    normalizedResult.sideEffectState = normalizedToolSideEffectState(result);
    normalizedResult.isError =
        normalizedResult.outcome != agent::ToolOutcome::Succeeded &&
        normalizedResult.outcome != agent::ToolOutcome::InProgress;
    activeRun_->toolRequestId.clear();
    const auto completedToolCallSignature =
        std::exchange(activeToolCallSignature_, QString{});
    const auto completedToolAction =
        std::exchange(activeToolAction_, std::nullopt);
    auto completedOperationKind = ToolOperationKind::Unknown;
    if (completedToolAction.has_value())
    {
        const auto definition = std::find_if(
            availableTools_.cbegin(), availableTools_.cend(),
            [&completedToolAction](const agent::ToolDefinition& candidate)
            {
                return candidate.qualifiedName == completedToolAction->toolName;
            });
        completedOperationKind =
            definition == availableTools_.cend()
                ? ToolOperationKind::Unknown
                : operationKind(*definition, dependencies_.toolRisk);
    }

    QString recoveryGuidance;

    completedToolCallHistory_.append(completedToolCallSignature);
    const auto outcome = normalizedResult.outcome;
    const auto inProgress = outcome == agent::ToolOutcome::InProgress;
    if (completedToolAction.has_value() && !inProgress)
    {
        if (completedOperationKind == ToolOperationKind::ReadOnly &&
            isDiscoveryToolName(completedToolAction->toolName))
            ++activeRun_->consecutiveDiscoveryCalls;
        else
            activeRun_->consecutiveDiscoveryCalls = 0;
    }
    if (inProgress)
    {
        pollableToolCallSignature_ = completedToolCallSignature;
        lastPollCompletedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    }
    else
    {
        pollableToolCallSignature_.clear();
        lastPollCompletedAtMs_ = 0;
    }
    auto evidenceSequence = 0;
    if (completedToolAction.has_value())
    {
        evidenceSequence = static_cast<int>(toolEvidence_.size() + 1);
        toolEvidence_.append(AgentContextCompactor::toolEvidence(
            evidenceSequence, *completedToolAction, normalizedResult));
        activeRun_->ledger.recordToolResult(
            evidenceSequence, *completedToolAction, normalizedResult,
            completedOperationKind);
        ++activeRun_->evidenceRevision;
        activeRun_->completionReviewsAtRevision = 0;
        activeRun_->completionReviewFailures = 0;
        activeRun_->completionPlanDriftRepairs = 0;
    }
    if (activeRun_->orderedTaskPlan && completedToolAction.has_value() &&
        outcome == agent::ToolOutcome::Succeeded && !inProgress &&
        evidenceSequence > 0 &&
        completedToolAction->completesPlanStep.value_or(false))
    {
        beginPlanStepReview(evidenceSequence);
        recoveryGuidance +=
            (recoveryGuidance.isEmpty() ? QString{} : QStringLiteral(" ")) +
            (currentExecutionTask() && currentExecutionTask()->awaitingReview()
                 ? QStringLiteral(
                       "The call proposed completion of the current task-plan "
                       "step. Review that exact step now; it has not advanced "
                       "yet.")
                 : QStringLiteral(
                       "The current task-plan step remains unfinished because "
                       "its mutation verification is unresolved."));
    }
    else if (activeRun_->orderedTaskPlan && completedToolAction.has_value() &&
             !inProgress && !completedToolAction->planStepId.isEmpty())
    {
        recoveryGuidance +=
            (recoveryGuidance.isEmpty() ? QString{} : QStringLiteral(" ")) +
            QStringLiteral(
                "Task-plan step '%1' remains current. Use the same "
                "plan_step_id for the next necessary call; do not advance to "
                "a later step yet.")
                .arg(completedToolAction->planStepId);
    }
    if (outcome != agent::ToolOutcome::Succeeded && !inProgress)
        lastFailedToolCallSignature_ = completedToolCallSignature;
    else
    {
        lastFailedToolCallSignature_.clear();
        if (outcome == agent::ToolOutcome::Succeeded)
            ++activeRun_->successfulToolResults;
    }
    activeRun_->stagnationRecoveries = 0;
    recordEvent(agent::EventType::ToolFinished,
                outcome == agent::ToolOutcome::Succeeded
                    ? QStringLiteral("Tool call completed.")
                : inProgress ? QStringLiteral("Tool call is still in progress.")
                             : normalizedResult.errorMessage,
                normalizedResult.serverId + QLatin1Char('.') +
                    normalizedResult.toolName,
                normalizedResult.result);
    auto resultMessage = AgentPromptBuilder::toolResultMessage(
        normalizedResult, evidenceSequence, activeRun_->ledger.snapshot(),
        activeRun_->ledger.unresolvedVerificationReason(), recoveryGuidance);
    if (auto* task = currentExecutionTask(); task && task->hasConversation())
        task->appendToolResult(
            std::move(resultMessage), toolEvidence_,
            activeRun_->ledger.snapshot(),
            activeRun_->ledger.unresolvedVerificationReason());
    else
        decisionMessages().append(std::move(resultMessage));

    const auto uncertainDispatch = normalizedResult.sideEffectState ==
                                   agent::ToolSideEffectState::Uncertain;
    const auto remoteFailure = outcome == agent::ToolOutcome::TransportFailed ||
                               outcome == agent::ToolOutcome::ProtocolFailed ||
                               outcome == agent::ToolOutcome::ServerFailed ||
                               outcome == agent::ToolOutcome::Cancelled;
    if (remoteFailure && uncertainDispatch &&
        completedOperationKind != ToolOperationKind::ReadOnly)
    {
        failRun(QStringLiteral("tool_side_effect_uncertain"),
                QStringLiteral(
                    "The tool request was dispatched, but its final outcome "
                    "is unknown. Verify the affected state before issuing "
                    "another mutating call."));
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
        activeRun_->readOnlyTransportRetries < 1 && completedToolAction)
    {
        ++activeRun_->readOnlyTransportRetries;
        const QJsonObject retryAction{
            {QStringLiteral("action"), QStringLiteral("call_tool")},
            {QStringLiteral("tool"), completedToolAction->toolName},
            {QStringLiteral("arguments"), completedToolAction->arguments}};
        decisionMessages().append(
            {chat::Role::Assistant,
             QString::fromUtf8(
                 QJsonDocument(retryAction).toJson(QJsonDocument::Compact))});
        executeTool(*completedToolAction);
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
    if (!activeRun_->orderedTaskPlan || activeRun_->executionTasks.empty())
        return nullptr;
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
    if (!activeRun_->orderedTaskPlan || activeRun_->executionTasks.empty())
        return nullptr;
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
                  evidenceStart);
    recordTaskStarted(task, index + 1,
                      static_cast<int>(activeRun_->executionTasks.size()));

    // Recovery windows belong to one task worker, not the whole plan.
    activeRun_->repairAttempts = 0;
    activeRun_->completionReviewFailures = 0;
    activeRun_->consecutiveValidationFailures = 0;
    activeRun_->stagnationRecoveries = 0;
    activeRun_->orderedPlanRepairs = 0;
    activeRun_->readOnlyTransportRetries = 0;
    activeRun_->consecutiveDiscoveryCalls = 0;
    lastFailedToolCallSignature_.clear();
    completedToolCallHistory_.clear();
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

void AgentController::requestDecision()
{
    if (!activeRun_) return;
    ++activeRun_->decisionCount;
    compactContextIfNeeded();
    decisionBytes_.clear();
    setState(AgentRun::State::Deciding);
    recordEvent(agent::EventType::DecisionStarted,
                QStringLiteral("Generating the next agent decision."));
    activeRun_->lastSubmittedCharacters = messageCharacters(decisionMessages());
    const auto outputTokens =
        qMax(minimumDecisionTokens, preset_.maxOutputTokens);
    if (auto* task = currentTask(); task && task->hasConversation())
        task->requestDecision(dependencies_.generate, preset_, outputTokens);
    else
        dependencies_.generate(decisionMessages(), preset_, outputTokens);
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
    if (activeRun_->orderedTaskPlan && !activeRun_->executionTasks.empty())
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
        {QStringLiteral("ordered"), activeRun_->orderedTaskPlan},
        {QStringLiteral("currentStepIndex"),
         activeRun_->currentExecutionTaskIndex},
        {QStringLiteral("evidenceRevision"), activeRun_->evidenceRevision},
        {QStringLiteral("lastReviewedEvidenceRevision"),
         activeRun_->lastReviewedEvidenceRevision},
        {QStringLiteral("awaitingReview"),
         activeRun_->awaitingCompletionReview},
        {QStringLiteral("awaitingPlanStepReview"),
         currentExecutionTask() && currentExecutionTask()->awaitingReview()},
        {QStringLiteral("currentStepEvidenceStart"),
         currentExecutionTask() ? currentExecutionTask()->evidenceStart() : 1},
        {QStringLiteral("pendingPlanStepEvidenceEnd"),
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

void AgentController::handleAction(const agent::Action& action,
                                   const QByteArray& rawAction,
                                   bool recordAction)
{
    if (!activeRun_) return;

    if (action.type == agent::ActionType::TaskPlan)
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral(
                "task_plan is valid only when the controller requests it."));
        return;
    }

    if (action.type == agent::ActionType::ReviewToolCall ||
        action.type == agent::ActionType::ReviewPlanStep)
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral("The requested task review is not active."));
        return;
    }

    if (action.type == agent::ActionType::Blocked)
    {
        blockRun(action.blockReason, action.content);
        return;
    }

    if (activeRun_->awaitingCompletionReview)
    {
        if (action.type == agent::ActionType::ReviewCompletion)
        {
            handleCompletionReview(action, rawAction);
            return;
        }
        if (action.type == agent::ActionType::Final)
        {
            retryCompletionReview(
                rawAction,
                QStringLiteral(
                    "A second final does not verify completion. Return "
                    "review_completion with per-step evidence."));
            return;
        }

        activeRun_->awaitingCompletionReview = false;
        activeRun_->pendingFinalCandidate.clear();
        activeRun_->completionReviewFailures = 0;
    }
    else if (action.type == agent::ActionType::ReviewCompletion)
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral(
                "review_completion is valid only after the controller asks "
                "to review a proposed final answer."));
        return;
    }

    if (action.type == agent::ActionType::Final)
    {
        if (!activeRun_->completionSteps.isEmpty())
            beginCompletionReview(action, rawAction);
        else
        {
            const auto unfinishedReason =
                unfinishedEvidenceReason(toolEvidence_, activeRun_->ledger);
            if (unfinishedReason.isEmpty())
                completeRun(action.content);
            else
                retryUnfinishedFinal(rawAction, unfinishedReason);
        }
        return;
    }

    ++activeRun_->toolActionAttempts;
    const auto recordDuplicateToolAction = [this, &action]
    {
        if (!activeRun_) return;
        ++activeRun_->duplicateToolActions;
        const auto definition = std::find_if(
            availableTools_.cbegin(), availableTools_.cend(),
            [&action](const agent::ToolDefinition& candidate)
            { return candidate.qualifiedName == action.toolName; });
        if (definition == availableTools_.cend()) return;
        const auto kind = operationKind(*definition, dependencies_.toolRisk);
        if (kind == ToolOperationKind::Mutation)
            ++activeRun_->duplicateMutationActions;
        if (kind == ToolOperationKind::ReadOnly &&
            isDiscoveryToolName(action.toolName))
            ++activeRun_->redundantDiscoveryCalls;
    };

    const auto tool =
        std::find_if(availableTools_.cbegin(), availableTools_.cend(),
                     [&action](const agent::ToolDefinition& candidate)
                     { return candidate.qualifiedName == action.toolName; });
    if (tool == availableTools_.cend())
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral("Tool is not available: %1").arg(action.toolName));
        return;
    }
    const auto candidateOperationKind =
        operationKind(*tool, dependencies_.toolRisk);
    if (activeRun_->orderedTaskPlan)
    {
        if (const auto* task = currentExecutionTask())
        {
            const auto taskError = task->validateToolAction(
                action, candidateOperationKind == ToolOperationKind::ReadOnly);
            if (!taskError.isEmpty())
            {
                retryOrderedPlanAction(rawAction, taskError);
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
                    "instead of executing another tool."));
            return;
        }
    }

    const auto signature = toolCallSignature(action);
    if (!lastFailedToolCallSignature_.isEmpty() &&
        signature == lastFailedToolCallSignature_)
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "The identical tool call already failed. Do not call it "
                "again. Return a final action now, or use meaningfully "
                "different arguments only when the user's request requires "
                "another attempt."));
        return;
    }

    const auto isStatusPoll = !pollableToolCallSignature_.isEmpty() &&
                              signature == pollableToolCallSignature_;
    const auto repeatedCallError =
        isStatusPoll
            ? QString{}
            : repeatedCompletedCallError(completedToolCallHistory_, signature);
    if (!repeatedCallError.isEmpty())
    {
        recordDuplicateToolAction();
        retryNoProgressAction(rawAction, repeatedCallError);
        return;
    }

    ++activeRun_->toolValidationAttempts;
    const auto validation =
        dependencies_.validateTool(action.toolName, action.arguments);
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
        retryInvalidToolAction(rawAction, issue, *tool, action.arguments);
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
        !isStatusPoll)
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
                .arg(maximumConsecutiveDiscoveryCalls));
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
            if (directive.code == QLatin1String("task_plan_repair"))
                recordEvent(agent::EventType::RecoveryStarted,
                            QStringLiteral("Repairing the task plan."));
            else if (directive.code == QLatin1String("summary_repair"))
                recordEvent(agent::EventType::RecoveryStarted,
                            QStringLiteral("Repairing the final summary."));
            else
                recordEvent(agent::EventType::RecoveryStarted,
                            QStringLiteral("Repairing the current task."));
            requestDecision();
            return;
        case AgentTask::Directive::Type::TasksCreated:
            acceptTaskPlan(directive.tasks, directive.ordered);
            return;
        case AgentTask::Directive::Type::CallTool:
            handleAction(directive.toolAction, directive.rawAction,
                         !directive.toolCallAlreadyRecorded);
            return;
        case AgentTask::Directive::Type::Completed:
            if (currentTask() &&
                currentTask()->kind() == AgentTask::Kind::Summary)
            {
                completeRun(directive.content);
                return;
            }
            if (!activeRun_->orderedTaskPlan)
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
                        failRun(QStringLiteral("completion_unverified"),
                                reason);
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
        case AgentTask::Directive::Type::Failed:
            failRun(directive.code, directive.detail);
            return;
        case AgentTask::Directive::Type::Continue:
            if (directive.code == QLatin1String("task_pending"))
                recordEvent(
                    agent::EventType::TaskStepUpdated,
                    QStringLiteral("Task plan step remains pending."), {},
                    {{QStringLiteral("stepId"),
                      currentExecutionTask() ? currentExecutionTask()->id()
                                             : QString{}}});
            else if (directive.code == QLatin1String("tool_call_rejected"))
                recordEvent(
                    agent::EventType::RecoveryStarted,
                    QStringLiteral(
                        "Rejected a tool call outside the current task."));
            requestDecision();
            return;
    }
}

void AgentController::dispatchTool(const agent::Action& action)
{
    if (!activeRun_) return;
    const auto decision = dependencies_.toolPolicy(action.toolName);
    if (decision == infrastructure::mcp::ToolDecision::Deny)
    {
        failRun(QStringLiteral("tool_denied"),
                QStringLiteral("Local policy denied the tool call."));
        return;
    }
    if (decision == infrastructure::mcp::ToolDecision::RequireApproval)
    {
        if (auto* task = currentExecutionTask()) task->awaitApproval();
        pendingApproval_ = action;
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

void AgentController::acceptTaskPlan(const QJsonArray& steps, bool ordered)
{
    if (!activeRun_) return;
    activeRun_->completionSteps = steps;
    activeRun_->orderedTaskPlan = ordered;
    activeRun_->currentExecutionTaskIndex = 0;
    activeRun_->taskPlanRequired = false;
    qInfo().noquote() << QStringLiteral(
                             "Agent task plan accepted: run=%1 steps=%2")
                             .arg(activeRun_->id)
                             .arg(activeRun_->completionSteps.size());
    recordEvent(
        agent::EventType::TaskPlanAccepted,
        QStringLiteral("Task plan accepted."), {},
        {{QStringLiteral("stepCount"), activeRun_->completionSteps.size()}});
    if (activeRun_->orderedTaskPlan)
    {
        std::vector<QSet<QString>> toolsRequiringSemanticReview(
            static_cast<std::size_t>(steps.size()));
        for (qsizetype index = 0; index < steps.size(); ++index)
        {
            const auto allowedTools =
                steps.at(index)
                    .toObject()
                    .value(QStringLiteral("allowed_tools"))
                    .toArray();
            for (const auto& toolValue : allowedTools)
            {
                const auto toolName = toolValue.toString();
                const auto definition = std::find_if(
                    availableTools_.cbegin(), availableTools_.cend(),
                    [&toolName](const agent::ToolDefinition& candidate)
                    { return candidate.qualifiedName == toolName; });
                if (definition == availableTools_.cend() ||
                    operationKind(*definition, dependencies_.toolRisk) ==
                        ToolOperationKind::ReadOnly)
                    continue;
                for (qsizetype otherIndex = 0; otherIndex < steps.size();
                     ++otherIndex)
                {
                    if (otherIndex == index) continue;
                    if (steps.at(otherIndex)
                            .toObject()
                            .value(QStringLiteral("allowed_tools"))
                            .toArray()
                            .contains(toolName))
                    {
                        toolsRequiringSemanticReview
                            .at(static_cast<std::size_t>(index))
                            .insert(toolName);
                        break;
                    }
                }
            }
        }

        activeRun_->executionTasks.clear();
        activeRun_->executionTasks.reserve(
            static_cast<std::size_t>(steps.size()));
        for (qsizetype index = 0; index < steps.size(); ++index)
            activeRun_->executionTasks.emplace_back(
                steps.at(index).toObject(),
                std::move(toolsRequiringSemanticReview.at(
                    static_cast<std::size_t>(index))));

        // Each ExecutionTask gets a fresh user turn for only its own
        // assignment and the completed-task background it may depend on.
        if (activeRun_->requestMessageIndex >= 0 &&
            activeRun_->requestMessageIndex <
                activeRun_->inferenceMessages.size())
            activeRun_->inferenceMessages.resize(
                activeRun_->requestMessageIndex);
        refreshExecutionTaskSnapshots();
        activateExecutionTask(0, static_cast<int>(toolEvidence_.size() + 1));
    }
    else
    {
        activeRun_->inferenceMessages = activeRun_->planningTask->messages();
        activeRun_->inferenceMessages.append(
            AgentPromptBuilder::taskPlanAcceptedMessage(
                activeRun_->completionSteps));
        activeRun_->requestMessageIndex =
            activeRun_->inferenceMessages.size() - 1;
    }
    requestDecision();
}

void AgentController::beginCompletionReview(const agent::Action& action,
                                            const QByteArray& rawAction)
{
    if (!activeRun_) return;
    ++activeRun_->completionReviewAttempts;
    if (activeRun_->lastReviewedEvidenceRevision ==
        activeRun_->evidenceRevision)
        ++activeRun_->completionReviewsAtRevision;
    else
    {
        activeRun_->lastReviewedEvidenceRevision = activeRun_->evidenceRevision;
        activeRun_->completionReviewsAtRevision = 1;
    }

    if (activeRun_->completionReviewsAtRevision >
        maximumCompletionReviewsPerEvidenceRevision)
    {
        failRun(QStringLiteral("completion_unverified"),
                QStringLiteral(
                    "The Agent proposed completion repeatedly without new tool "
                    "evidence after an unfinished completion review."));
        return;
    }

    activeRun_->pendingFinalCandidate = action.content;
    activeRun_->awaitingCompletionReview = true;
    activeRun_->completionReviewFailures = 0;
    qInfo().noquote()
        << QStringLiteral(
               "Agent completion review started: run=%1 evidenceRevision=%2 "
               "attemptAtRevision=%3 steps=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->evidenceRevision)
               .arg(activeRun_->completionReviewsAtRevision)
               .arg(activeRun_->completionSteps.size());
    decisionMessages().append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    decisionMessages().append(AgentPromptBuilder::completionReviewMessage(
        activeRun_->userRequest, activeRun_->completionSteps, toolEvidence_,
        activeRun_->ledger.snapshot(),
        activeRun_->ledger.unresolvedVerificationReason()));
    requestDecision();
}

QString AgentController::validateCompletionReview(
    const agent::Action& action) const
{
    if (!activeRun_ || !activeRun_->awaitingCompletionReview ||
        activeRun_->pendingFinalCandidate.isEmpty())
        return QStringLiteral("No proposed final answer is awaiting review.");

    for (const auto& plannedValue : activeRun_->completionSteps)
    {
        const auto planned = plannedValue.toObject();
        const auto plannedId = planned.value(QStringLiteral("id")).toString();
        auto found = false;
        for (const auto& reviewedValue : action.completionSteps)
        {
            const auto reviewed = reviewedValue.toObject();
            if (reviewed.value(QStringLiteral("id")).toString() != plannedId)
                continue;
            found = true;
            if (reviewed.value(QStringLiteral("description")) !=
                    planned.value(QStringLiteral("description")) ||
                reviewed.value(QStringLiteral("requires_tool")) !=
                    planned.value(QStringLiteral("requires_tool")))
                return QStringLiteral(
                           "Completion step %1 changed its recorded "
                           "description or requires_tool value.")
                    .arg(plannedId);
            const auto recordedStatus =
                planned.value(QStringLiteral("status")).toString();
            const auto reviewedStatus =
                reviewed.value(QStringLiteral("status")).toString();
            if (recordedStatus == QLatin1String("satisfied") &&
                reviewedStatus != QLatin1String("satisfied"))
                return QStringLiteral(
                           "Completion step %1 was already verified by the "
                           "controller and cannot be moved back to %2.")
                    .arg(plannedId, reviewedStatus);
            break;
        }
        if (!found)
            return QStringLiteral("Completion review omitted recorded step %1.")
                .arg(plannedId);
    }

    auto priorStepUnfinished = false;
    for (const auto& reviewedValue : action.completionSteps)
    {
        const auto reviewed = reviewedValue.toObject();
        const auto id = reviewed.value(QStringLiteral("id")).toString();
        const auto status = reviewed.value(QStringLiteral("status")).toString();
        const auto requiresTool =
            reviewed.value(QStringLiteral("requires_tool")).toBool();
        const auto sequences =
            reviewed.value(QStringLiteral("evidence")).toArray();
        if (priorStepUnfinished && status == QLatin1String("satisfied"))
            return QStringLiteral(
                       "Completion step %1 cannot be satisfied before all "
                       "preceding task-plan steps are satisfied.")
                .arg(id);
        auto hasTerminalSuccess = false;
        auto citesUnverifiedMutation = false;
        for (const auto& sequenceValue : sequences)
        {
            const auto sequence = sequenceValue.toInt();
            auto found = false;
            for (const auto& evidence : toolEvidence_)
            {
                if (evidence.value(QStringLiteral("sequence")).toInt() !=
                    sequence)
                    continue;
                found = true;
                hasTerminalSuccess =
                    hasTerminalSuccess ||
                    (evidence.value(QStringLiteral("outcome")).toString() ==
                         QLatin1String("success") &&
                     evidence.value(QStringLiteral("terminal")).toBool());
                citesUnverifiedMutation =
                    citesUnverifiedMutation ||
                    activeRun_->ledger.evidenceRequiresVerification(sequence);
                break;
            }
            if (!found)
                return QStringLiteral(
                           "Completion step %1 cites unknown tool evidence "
                           "sequence %2.")
                    .arg(id)
                    .arg(sequence);
        }
        if (status == QLatin1String("satisfied") && requiresTool &&
            !hasTerminalSuccess)
            return QStringLiteral(
                       "Completion step %1 requires successful terminal tool "
                       "evidence.")
                .arg(id);
        if (status == QLatin1String("satisfied") && citesUnverifiedMutation)
            return QStringLiteral(
                       "Completion step %1 cites a mutation whose read-back "
                       "verification is still unresolved.")
                .arg(id);
        if (status != QLatin1String("satisfied")) priorStepUnfinished = true;
    }
    if (action.completionVerdict == QLatin1String("complete") &&
        activeRun_->ledger.hasUnresolvedVerification())
        return activeRun_->ledger.unresolvedVerificationReason();
    return {};
}

void AgentController::beginPlanStepReview(int evidenceSequence)
{
    auto* task = currentExecutionTask();
    if (!activeRun_ || !task || activeRun_->ledger.hasUnresolvedVerification())
        return;
    task->beginReview(evidenceSequence);
}

bool AgentController::hasSufficientCompletionEvidence() const
{
    if (!activeRun_ || activeRun_->pendingFinalCandidate.isEmpty() ||
        activeRun_->completionSteps.isEmpty() ||
        !unfinishedEvidenceReason(toolEvidence_, activeRun_->ledger).isEmpty())
        return false;

    auto requiredToolSteps = 0;
    for (const auto& value : activeRun_->completionSteps)
        if (value.toObject().value(QStringLiteral("requires_tool")).toBool())
            ++requiredToolSteps;

    auto successfulTerminalEvidence = 0;
    for (const auto& evidence : toolEvidence_)
        if (evidence.value(QStringLiteral("outcome")).toString() ==
                QLatin1String("success") &&
            evidence.value(QStringLiteral("terminal")).toBool())
            ++successfulTerminalEvidence;

    return successfulTerminalEvidence >= requiredToolSteps;
}

void AgentController::handleCompletionReview(const agent::Action& action,
                                             const QByteArray& rawAction)
{
    if (!activeRun_) return;
    auto normalizedAction = action;
    QStringList omittedStepIds;
    QStringList unexpectedStepIds;
    QJsonArray authoritativeSteps;
    for (const auto& plannedValue : activeRun_->completionSteps)
    {
        const auto planned = plannedValue.toObject();
        const auto plannedId = planned.value(QStringLiteral("id")).toString();
        auto reviewed = std::find_if(action.completionSteps.cbegin(),
                                     action.completionSteps.cend(),
                                     [&plannedId](const QJsonValue& value)
                                     {
                                         return value.toObject()
                                                    .value(QStringLiteral("id"))
                                                    .toString() == plannedId;
                                     });
        if (reviewed != action.completionSteps.cend())
        {
            auto reviewedStep = reviewed->toObject();
            reviewedStep.insert(QStringLiteral("description"),
                                planned.value(QStringLiteral("description")));
            reviewedStep.insert(QStringLiteral("requires_tool"),
                                planned.value(QStringLiteral("requires_tool")));
            reviewedStep.insert(QStringLiteral("allowed_tools"),
                                planned.value(QStringLiteral("allowed_tools")));
            authoritativeSteps.append(reviewedStep);
            continue;
        }

        auto restored = planned;
        if (!restored.contains(QStringLiteral("status")))
            restored.insert(QStringLiteral("status"),
                            QStringLiteral("pending"));
        if (!restored.contains(QStringLiteral("evidence")))
            restored.insert(QStringLiteral("evidence"), QJsonArray{});
        authoritativeSteps.append(restored);
        omittedStepIds.append(plannedId);
    }
    for (const auto& reviewedValue : action.completionSteps)
    {
        const auto reviewedId =
            reviewedValue.toObject().value(QStringLiteral("id")).toString();
        const auto recorded =
            std::any_of(activeRun_->completionSteps.cbegin(),
                        activeRun_->completionSteps.cend(),
                        [&reviewedId](const QJsonValue& value)
                        {
                            return value.toObject()
                                       .value(QStringLiteral("id"))
                                       .toString() == reviewedId;
                        });
        if (!recorded) unexpectedStepIds.append(reviewedId);
    }
    if (!omittedStepIds.isEmpty())
    {
        if (activeRun_->completionPlanDriftRepairs >= 1)
        {
            retryCompletionReview(
                rawAction,
                QStringLiteral(
                    "Completion review repeatedly replaced or omitted "
                    "recorded task-plan steps."));
            return;
        }
        ++activeRun_->completionPlanDriftRepairs;
        decisionMessages().append(
            {chat::Role::Assistant, QString::fromUtf8(rawAction)});
        decisionMessages().append(
            AgentPromptBuilder::completionPlanDriftMessage(
                activeRun_->completionSteps));
        recordEvent(
            agent::EventType::RecoveryStarted,
            QStringLiteral(
                "Restoring the completion review to the recorded task plan."),
            {},
            {{QStringLiteral("omittedStepCount"), omittedStepIds.size()},
             {QStringLiteral("unexpectedStepCount"),
              unexpectedStepIds.size()}});
        requestDecision();
        return;
    }

    for (const auto& reviewedValue : action.completionSteps)
    {
        const auto reviewedId =
            reviewedValue.toObject().value(QStringLiteral("id")).toString();
        if (unexpectedStepIds.contains(reviewedId))
            authoritativeSteps.append(reviewedValue);
    }
    normalizedAction.completionSteps = authoritativeSteps;

    const auto validationError = validateCompletionReview(normalizedAction);
    if (!validationError.isEmpty())
    {
        retryCompletionReview(rawAction, validationError);
        return;
    }

    activeRun_->completionSteps = normalizedAction.completionSteps;
    activeRun_->currentExecutionTaskIndex = activeRun_->completionSteps.size();
    for (qsizetype index = 0; index < activeRun_->completionSteps.size();
         ++index)
    {
        const auto status = activeRun_->completionSteps.at(index)
                                .toObject()
                                .value(QStringLiteral("status"))
                                .toString();
        if (status != QLatin1String("satisfied") &&
            status != QLatin1String("blocked"))
        {
            activeRun_->currentExecutionTaskIndex = static_cast<int>(index);
            break;
        }
    }
    ++activeRun_->completionReviewSuccesses;
    activeRun_->awaitingCompletionReview = false;
    activeRun_->completionReviewFailures = 0;
    qInfo().noquote()
        << QStringLiteral(
               "Agent completion review accepted: run=%1 verdict=%2 steps=%3")
               .arg(activeRun_->id, normalizedAction.completionVerdict)
               .arg(activeRun_->completionSteps.size());
    recordEvent(
        agent::EventType::TaskStepUpdated,
        QStringLiteral("Task step status updated."), {},
        {{QStringLiteral("verdict"), normalizedAction.completionVerdict},
         {QStringLiteral("stepCount"), activeRun_->completionSteps.size()}});
    if (normalizedAction.completionVerdict == QLatin1String("complete"))
    {
        const auto content =
            std::exchange(activeRun_->pendingFinalCandidate, QString{});
        completeRun(content);
        return;
    }
    activeRun_->pendingFinalCandidate.clear();
    if (normalizedAction.completionVerdict == QLatin1String("blocked"))
    {
        completeRun(normalizedAction.completionDetail);
        return;
    }

    decisionMessages().append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    decisionMessages().append(AgentPromptBuilder::completionContinuationMessage(
        activeRun_->completionSteps, normalizedAction.completionDetail));
    requestDecision();
}

void AgentController::executeTool(const agent::Action& action)
{
    if (!activeRun_) return;
    if (auto* task = currentExecutionTask()) task->awaitTool();
    const auto signature = toolCallSignature(action);
    if (!pollableToolCallSignature_.isEmpty() &&
        signature == pollableToolCallSignature_ && lastPollCompletedAtMs_ > 0)
    {
        const auto elapsed =
            QDateTime::currentMSecsSinceEpoch() - lastPollCompletedAtMs_;
        const auto remaining = minimumPollIntervalMilliseconds - elapsed;
        if (remaining > 0)
        {
            pendingPollAction_ = action;
            setState(AgentRun::State::ExecutingTool);
            pollTimer_->start(static_cast<int>(remaining));
            return;
        }
    }
    if (!pollableToolCallSignature_.isEmpty() &&
        signature == pollableToolCallSignature_)
        ++activeRun_->pollRequests;
    ++activeRun_->executedToolCalls;
    activeToolAction_ = action;
    activeToolCallSignature_ = signature;
    setState(AgentRun::State::ExecutingTool);
    recordEvent(agent::EventType::ToolStarted,
                QStringLiteral("Tool call started."), action.toolName,
                action.arguments);
    activeRun_->toolRequestId =
        dependencies_.callTool(action.toolName, action.arguments);
    if (activeRun_->toolRequestId.isEmpty())
        failRun(QStringLiteral("tool_call_failed"),
                QStringLiteral("Tool call could not be started."));
}

void AgentController::executePendingPoll()
{
    if (!hasActiveRun() || !pendingPollAction_.has_value()) return;
    const auto action = std::exchange(pendingPollAction_, std::nullopt);
    executeTool(*action);
}

void AgentController::retryCompletionReview(const QByteArray& rawAction,
                                            const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->completionReviewFailures >= 1)
    {
        qWarning().noquote()
            << QStringLiteral(
                   "Agent completion review failed: run=%1 reason=%2 "
                   "action=%3")
                   .arg(activeRun_->id, singleLine(errorMessage),
                        singleLine(QString::fromUtf8(rawAction)).left(2'048));
        if (hasSufficientCompletionEvidence())
        {
            const auto content =
                std::exchange(activeRun_->pendingFinalCandidate, QString{});
            activeRun_->awaitingCompletionReview = false;
            recordEvent(
                agent::EventType::Warning,
                QStringLiteral(
                    "Completion review formatting failed after correction; "
                    "using the prepared final answer because successful "
                    "terminal evidence covers every tool-required step."));
            completeRun(content);
            return;
        }
        failRun(QStringLiteral("completion_unverified"), errorMessage);
        return;
    }
    ++activeRun_->completionReviewFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent completion review correction: run=%1 attempt=%2 "
               "reason=%3 action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->completionReviewFailures)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    decisionMessages().append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    decisionMessages().append(
        AgentPromptBuilder::completionReviewCorrectionMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing the completion review."));
    requestDecision();
}

void AgentController::retryUnfinishedFinal(const QByteArray& rawAction,
                                           const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->completionReviewFailures >= 1)
    {
        qWarning().noquote()
            << QStringLiteral(
                   "Agent unfinished final failed: run=%1 reason=%2 action=%3")
                   .arg(activeRun_->id, singleLine(errorMessage),
                        singleLine(QString::fromUtf8(rawAction)).left(2'048));
        failRun(QStringLiteral("completion_unverified"), errorMessage);
        return;
    }
    ++activeRun_->completionReviewFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent unfinished final correction: run=%1 attempt=%2 "
               "reason=%3 action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->completionReviewFailures)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    decisionMessages().append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    decisionMessages().append(
        AgentPromptBuilder::unfinishedFinalMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Checking unfinished task steps."));
    requestDecision();
}

void AgentController::retryInvalidAction(const QByteArray& rawAction,
                                         const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->awaitingCompletionReview)
    {
        retryCompletionReview(rawAction, errorMessage);
        return;
    }
    if (activeRun_->orderedTaskPlan)
    {
        retryOrderedPlanAction(rawAction, errorMessage);
        return;
    }
    if (activeRun_->repairAttempts >= 1)
    {
        failRun(QStringLiteral("invalid_agent_action"), errorMessage);
        return;
    }
    ++activeRun_->repairAttempts;
    qWarning().noquote()
        << QStringLiteral(
               "Agent action correction: run=%1 attempt=%2 reason=%3 "
               "action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->repairAttempts)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    decisionMessages().append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    decisionMessages().append(
        AgentPromptBuilder::correctionMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing an invalid agent action."));
    requestDecision();
}

void AgentController::retryInvalidToolAction(
    const QByteArray& rawAction, const agent::ToolValidationIssue& issue,
    const agent::ToolDefinition& tool, const QJsonObject& arguments)
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
    decisionMessages().append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    decisionMessages().append(
        AgentPromptBuilder::toolValidationCorrectionMessage(tool, arguments,
                                                            issue));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing invalid tool arguments."),
                tool.qualifiedName);
    requestDecision();
}

void AgentController::retryNoProgressAction(const QByteArray& rawAction,
                                            const QString& errorMessage)
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
    decisionMessages().append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    decisionMessages().append(
        AgentPromptBuilder::noProgressMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Recovering from a repeated action."));
    requestDecision();
}

void AgentController::retryOrderedPlanAction(const QByteArray& rawAction,
                                             const QString& errorMessage)
{
    if (!activeRun_) return;
    auto* task = currentExecutionTask();
    if (!task || !task->repairAction(rawAction, errorMessage))
    {
        failRun(QStringLiteral("invalid_agent_action"), errorMessage);
        return;
    }
    ++activeRun_->orderedPlanRepairs;
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing an ordered task-plan action."));
    requestDecision();
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
    QString kind;
    switch (task.kind())
    {
        case AgentTask::Kind::Planning:
            kind = QStringLiteral("planning");
            break;
        case AgentTask::Kind::Execution:
            kind = QStringLiteral("execution");
            break;
        case AgentTask::Kind::Summary:
            kind = QStringLiteral("summary");
            break;
    }

    QJsonObject data{{QStringLiteral("taskKind"), kind},
                     {QStringLiteral("taskId"), task.id()},
                     {QStringLiteral("description"), task.description()}};
    if (ordinal > 0) data.insert(QStringLiteral("taskIndex"), ordinal);
    if (total > 0) data.insert(QStringLiteral("taskCount"), total);
    recordEvent(agent::EventType::TaskStarted,
                QStringLiteral("%1 task started.").arg(kind), {}, data);
}

void AgentController::emitProgressChanged()
{
    if (!activeRun_) return;
    emit progressChanged(progressSnapshot());
}

void AgentController::completeRun(const QString& content)
{
    if (!activeRun_) return;
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
    pendingPollAction_.reset();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    recordEvent(agent::EventType::Completed,
                QStringLiteral("Agent run completed."));
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, {}, {});
}

void AgentController::blockRun(const QString& reason, const QString& content)
{
    if (!activeRun_) return;
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
    pendingPollAction_.reset();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
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
    const auto previousState = state_;
    const auto toolRequestId = activeRun_->toolRequestId;
    activeRun_->finishCode = code;
    activeRun_->finishMessage = message;
    if (auto* task = currentTask())
    {
        task->fail();
        if (activeRun_->orderedTaskPlan &&
            activeRun_->currentExecutionTaskIndex >= 0 &&
            activeRun_->currentExecutionTaskIndex <
                static_cast<int>(activeRun_->executionTasks.size()))
            refreshExecutionTaskSnapshots();
    }
    setState(AgentRun::State::Failed);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    pendingApproval_.reset();
    activeToolAction_.reset();
    pendingPollAction_.reset();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    activeRun_->toolRequestId.clear();
    if (previousState == AgentRun::State::Deciding &&
        dependencies_.cancelGeneration)
        dependencies_.cancelGeneration();
    if (previousState == AgentRun::State::ExecutingTool &&
        !toolRequestId.isEmpty() && dependencies_.cancelTool)
        dependencies_.cancelTool(toolRequestId);
    recordEvent(agent::EventType::Failed, message);
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, code, message);
}
}  // namespace qtllm::application
