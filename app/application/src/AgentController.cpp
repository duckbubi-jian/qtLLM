#include "AgentController.hpp"

#include "AgentContextCompactor.hpp"
#include "AgentPromptBuilder.hpp"
#include "ToolResultStatus.hpp"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
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
constexpr qsizetype maximumDetectedCycleLength = 4;
constexpr qsizetype maximumLoggedEventDataBytes = 4'096;

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

QString unfinishedEvidenceReason(const QList<QJsonObject>& toolEvidence)
{
    if (toolEvidence.isEmpty()) return {};
    const auto& latest = toolEvidence.constLast();
    if (latest.value(QStringLiteral("outcome")).toString() ==
        QLatin1String("error"))
        return QStringLiteral(
            "The most recent tool call failed, so the requested operation "
            "does not yet have successful evidence.");
    if (!latest.value(QStringLiteral("terminal")).toBool())
        return QStringLiteral(
            "The most recent tool result is still running or pending and is "
            "not terminal evidence.");
    return {};
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
    runTimer_->setSingleShot(true);
    runTimer_->setInterval(runTimeoutMilliseconds);
    connect(runTimer_, &QTimer::timeout, this,
            &AgentController::notifyLongRunning);
    pollTimer_->setSingleShot(true);
    connect(pollTimer_, &QTimer::timeout, this,
            &AgentController::executePendingPoll);
}

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
    emit userRequestAccepted(activeRun_->id, request);
    requestDecision();
    return true;
}

void AgentController::cancel()
{
    if (!hasActiveRun()) return;
    const auto previousState = state_;
    const auto toolRequestId = activeRun_->toolRequestId;
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

void AgentController::receiveToken(const QByteArray& bytes)
{
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding ||
        bytes.isEmpty())
        return;
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

    agent::Action action;
    QString errorMessage;
    if (!agent::parseAction(decisionBytes_, action, errorMessage))
    {
        retryInvalidAction(decisionBytes_, errorMessage);
        return;
    }
    handleAction(action, decisionBytes_);
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
    activeRun_->toolRequestId.clear();
    const auto completedToolCallSignature =
        std::exchange(activeToolCallSignature_, QString{});
    const auto completedToolAction =
        std::exchange(activeToolAction_, std::nullopt);
    completedToolCallHistory_.append(completedToolCallSignature);
    if (toolResultIndicatesInProgress(result))
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
            evidenceSequence, *completedToolAction, result));
        ++activeRun_->evidenceRevision;
        activeRun_->completionReviewsAtRevision = 0;
        activeRun_->completionReviewFailures = 0;
    }
    if (result.isError)
        lastFailedToolCallSignature_ = completedToolCallSignature;
    else
    {
        lastFailedToolCallSignature_.clear();
        ++activeRun_->successfulToolResults;
    }
    activeRun_->stagnationRecoveries = 0;
    recordEvent(agent::EventType::ToolFinished,
                result.isError ? result.errorMessage
                               : QStringLiteral("Tool call completed."),
                result.serverId + QLatin1Char('.') + result.toolName,
                result.result);
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::toolResultMessage(result, evidenceSequence));
    requestDecision();
}

bool AgentController::isTerminal(AgentRun::State state)
{
    return state == AgentRun::State::Completed ||
           state == AgentRun::State::Cancelled ||
           state == AgentRun::State::Failed;
}

void AgentController::requestDecision()
{
    if (!activeRun_) return;
    compactContextIfNeeded();
    decisionBytes_.clear();
    setState(AgentRun::State::Deciding);
    recordEvent(agent::EventType::DecisionStarted,
                QStringLiteral("Generating the next agent decision."));
    activeRun_->lastSubmittedCharacters =
        messageCharacters(activeRun_->inferenceMessages);
    dependencies_.generate(
        activeRun_->inferenceMessages, preset_,
        qMax(minimumDecisionTokens, preset_.maxOutputTokens));
}

void AgentController::compactContextIfNeeded()
{
    if (!activeRun_) return;
    auto estimatedPromptTokens = activeRun_->lastPromptTokens;
    const auto currentCharacters =
        messageCharacters(activeRun_->inferenceMessages);
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
    const QJsonObject completionState{
        {QStringLiteral("steps"), activeRun_->completionSteps},
        {QStringLiteral("evidenceRevision"), activeRun_->evidenceRevision},
        {QStringLiteral("lastReviewedEvidenceRevision"),
         activeRun_->lastReviewedEvidenceRevision},
        {QStringLiteral("awaitingReview"),
         activeRun_->awaitingCompletionReview}};
    const auto result = AgentContextCompactor::compact(
        activeRun_->inferenceMessages, activeRun_->requestMessageIndex,
        activeRun_->userRequest, toolEvidence_, completionState, preset_);
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
                                   const QByteArray& rawAction)
{
    if (!activeRun_) return;

    if (activeRun_->taskPlanRequired)
    {
        if (action.type == agent::ActionType::TaskPlan)
            acceptTaskPlan(action, rawAction);
        else
            retryTaskPlan(
                rawAction,
                QStringLiteral(
                    "A task_plan is required before executing or completing "
                    "this multi-step request."));
        return;
    }

    if (action.type == agent::ActionType::TaskPlan)
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral(
                "task_plan is valid only when the controller requests it."));
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
                unfinishedEvidenceReason(toolEvidence_);
            if (unfinishedReason.isEmpty())
                completeRun(action.content);
            else
                retryUnfinishedFinal(rawAction, unfinishedReason);
        }
        return;
    }

    const auto signature = toolCallSignature(action);
    if (!lastFailedToolCallSignature_.isEmpty() &&
        signature == lastFailedToolCallSignature_)
    {
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
        retryNoProgressAction(rawAction, repeatedCallError);
        return;
    }

    const auto available =
        std::any_of(availableTools_.cbegin(), availableTools_.cend(),
                    [&action](const agent::ToolDefinition& tool)
                    { return tool.qualifiedName == action.toolName; });
    QString validationError;
    if (!available)
        validationError =
            QStringLiteral("Tool is not available: %1").arg(action.toolName);
    else if (!dependencies_.validateTool(action.toolName, action.arguments,
                                         validationError))
    {
        if (validationError.isEmpty())
            validationError =
                QStringLiteral("Tool arguments failed local validation.");
    }
    if (!validationError.isEmpty())
    {
        retryInvalidAction(rawAction, validationError);
        return;
    }
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    const auto decision = dependencies_.toolPolicy(action.toolName);
    if (decision == infrastructure::mcp::ToolDecision::Deny)
    {
        failRun(QStringLiteral("tool_denied"),
                QStringLiteral("Local policy denied the tool call."));
        return;
    }
    if (decision == infrastructure::mcp::ToolDecision::RequireApproval)
    {
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

void AgentController::acceptTaskPlan(const agent::Action& action,
                                     const QByteArray& rawAction)
{
    if (!activeRun_) return;
    activeRun_->completionSteps = action.completionSteps;
    activeRun_->taskPlanRequired = false;
    activeRun_->taskPlanFailures = 0;
    qInfo().noquote() << QStringLiteral(
                             "Agent task plan accepted: run=%1 steps=%2")
                             .arg(activeRun_->id)
                             .arg(activeRun_->completionSteps.size());
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::taskPlanAcceptedMessage(
            activeRun_->completionSteps));
    requestDecision();
}

void AgentController::beginCompletionReview(const agent::Action& action,
                                            const QByteArray& rawAction)
{
    if (!activeRun_) return;
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
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::completionReviewMessage(activeRun_->userRequest,
                                                    activeRun_->completionSteps,
                                                    toolEvidence_));
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
            break;
        }
        if (!found)
            return QStringLiteral("Completion review omitted recorded step %1.")
                .arg(plannedId);
    }

    for (const auto& reviewedValue : action.completionSteps)
    {
        const auto reviewed = reviewedValue.toObject();
        const auto id = reviewed.value(QStringLiteral("id")).toString();
        const auto status = reviewed.value(QStringLiteral("status")).toString();
        const auto requiresTool =
            reviewed.value(QStringLiteral("requires_tool")).toBool();
        const auto sequences =
            reviewed.value(QStringLiteral("evidence")).toArray();
        auto hasTerminalSuccess = false;
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
    }
    return {};
}

bool AgentController::hasSufficientCompletionEvidence() const
{
    if (!activeRun_ || activeRun_->pendingFinalCandidate.isEmpty() ||
        activeRun_->completionSteps.isEmpty() ||
        !unfinishedEvidenceReason(toolEvidence_).isEmpty())
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
    const auto validationError = validateCompletionReview(action);
    if (!validationError.isEmpty())
    {
        retryCompletionReview(rawAction, validationError);
        return;
    }

    activeRun_->completionSteps = action.completionSteps;
    activeRun_->awaitingCompletionReview = false;
    activeRun_->completionReviewFailures = 0;
    qInfo().noquote()
        << QStringLiteral(
               "Agent completion review accepted: run=%1 verdict=%2 steps=%3")
               .arg(activeRun_->id, action.completionVerdict)
               .arg(activeRun_->completionSteps.size());
    if (action.completionVerdict == QLatin1String("complete"))
    {
        const auto content =
            std::exchange(activeRun_->pendingFinalCandidate, QString{});
        completeRun(content);
        return;
    }
    activeRun_->pendingFinalCandidate.clear();
    if (action.completionVerdict == QLatin1String("blocked"))
    {
        completeRun(action.completionDetail);
        return;
    }

    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::completionContinuationMessage(
            activeRun_->completionSteps, action.completionDetail));
    requestDecision();
}

void AgentController::executeTool(const agent::Action& action)
{
    if (!activeRun_) return;
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

void AgentController::retryTaskPlan(const QByteArray& rawAction,
                                    const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->taskPlanFailures >= 1)
    {
        qWarning().noquote()
            << QStringLiteral(
                   "Agent task plan failed: run=%1 reason=%2 action=%3")
                   .arg(activeRun_->id, singleLine(errorMessage),
                        singleLine(QString::fromUtf8(rawAction)).left(2'048));
        failRun(QStringLiteral("task_plan_failed"), errorMessage);
        return;
    }
    ++activeRun_->taskPlanFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent task plan correction: run=%1 attempt=%2 reason=%3 "
               "action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->taskPlanFailures)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::taskPlanCorrectionMessage(errorMessage));
    requestDecision();
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
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::completionReviewCorrectionMessage(errorMessage));
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
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::unfinishedFinalMessage(errorMessage));
    requestDecision();
}

void AgentController::retryInvalidAction(const QByteArray& rawAction,
                                         const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->taskPlanRequired)
    {
        retryTaskPlan(rawAction, errorMessage);
        return;
    }
    if (activeRun_->awaitingCompletionReview)
    {
        retryCompletionReview(rawAction, errorMessage);
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
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::correctionMessage(errorMessage));
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
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::noProgressMessage(errorMessage));
    requestDecision();
}

void AgentController::setState(AgentRun::State state)
{
    if (state_ == state) return;
    state_ = state;
    if (activeRun_) activeRun_->state = state;
    emit stateChanged(state_);
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
}

void AgentController::completeRun(const QString& content)
{
    if (!activeRun_) return;
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
    emit runFinished(activeRun_->id, state_, {}, {});
}

void AgentController::failRun(const QString& code, const QString& message)
{
    if (!activeRun_ || isTerminal(state_)) return;
    const auto previousState = state_;
    const auto toolRequestId = activeRun_->toolRequestId;
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
    emit runFinished(activeRun_->id, state_, code, message);
}
}  // namespace qtllm::application
