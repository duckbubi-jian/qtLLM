#include "AgentController.hpp"

#include "AgentPromptBuilder.hpp"

#include <QJsonDocument>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace qtllm::application
{
namespace
{
constexpr auto maximumToolCalls = 5;
constexpr auto maximumDecisionBytes = 65'536;
constexpr auto maximumDecisionTokens = 256;
constexpr auto runTimeoutMilliseconds = 120'000;
}  // namespace

AgentController::AgentController(Dependencies dependencies, QObject* parent)
    : QObject(parent), dependencies_(std::move(dependencies))
{
    runTimer_.setSingleShot(true);
    runTimer_.setInterval(runTimeoutMilliseconds);
    connect(&runTimer_, &QTimer::timeout, this,
            [this]
            {
                if (hasActiveRun())
                    failRun(QStringLiteral("agent_timeout"),
                            QStringLiteral("Agent run exceeded two minutes."));
            });
}

bool AgentController::start(const QString& userRequest,
                            const models::InferencePreset& preset,
                            const QList<agent::ToolDefinition>& tools)
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
    run.inferenceMessages = AgentPromptBuilder::initialMessages(request, tools);
    activeRun_ = std::move(run);
    preset_ = preset;
    availableTools_ = tools;
    pendingApproval_.reset();
    decisionBytes_.clear();
    runTimer_.start();

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
    runTimer_.stop();
    pendingApproval_.reset();
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
    Q_UNUSED(metrics)
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding) return;
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
    recordEvent(agent::EventType::ToolFinished,
                result.isError ? result.errorMessage
                               : QStringLiteral("Tool call completed."),
                result.serverId + QLatin1Char('.') + result.toolName,
                result.result);
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::toolResultMessage(result));
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
    decisionBytes_.clear();
    setState(AgentRun::State::Deciding);
    recordEvent(agent::EventType::DecisionStarted,
                QStringLiteral("Generating the next agent decision."));
    dependencies_.generate(activeRun_->inferenceMessages, preset_,
                           maximumDecisionTokens);
}

void AgentController::handleAction(const agent::Action& action,
                                   const QByteArray& rawAction)
{
    if (!activeRun_) return;
    if (action.type == agent::ActionType::Final)
    {
        completeRun(action.content);
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
    if (activeRun_->toolCallCount >= maximumToolCalls)
    {
        failRun(QStringLiteral("tool_limit"),
                QStringLiteral("Agent reached the five tool call limit."));
        return;
    }

    const auto signature =
        action.toolName + QLatin1Char(':') +
        QString::fromUtf8(
            QJsonDocument(action.arguments).toJson(QJsonDocument::Compact));
    if (signature == activeRun_->lastToolCallSignature)
        ++activeRun_->consecutiveToolCallCount;
    else
    {
        activeRun_->lastToolCallSignature = signature;
        activeRun_->consecutiveToolCallCount = 1;
    }
    if (activeRun_->consecutiveToolCallCount > 2)
    {
        failRun(QStringLiteral("tool_loop"),
                QStringLiteral(
                    "Agent repeated the same tool call too many times."));
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

void AgentController::executeTool(const agent::Action& action)
{
    if (!activeRun_) return;
    ++activeRun_->toolCallCount;
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

void AgentController::retryInvalidAction(const QByteArray& rawAction,
                                         const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->repairAttempts >= 1)
    {
        failRun(QStringLiteral("invalid_agent_action"), errorMessage);
        return;
    }
    ++activeRun_->repairAttempts;
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::correctionMessage(errorMessage));
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
    emit eventRecorded(event);
}

void AgentController::completeRun(const QString& content)
{
    if (!activeRun_) return;
    setState(AgentRun::State::GeneratingAnswer);
    recordEvent(agent::EventType::AnswerStarted,
                QStringLiteral("Preparing final answer."));
    emit finalAnswerReady(activeRun_->id, content);
    setState(AgentRun::State::Completed);
    runTimer_.stop();
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
    runTimer_.stop();
    pendingApproval_.reset();
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
