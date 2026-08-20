#include "AgentToolRuntime.hpp"

#include "ToolResultStatus.hpp"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <utility>

namespace qtllm::application
{
namespace
{
constexpr qsizetype maximumDetectedCycleLength = 4;
constexpr auto minimumPollIntervalMilliseconds = 1'000;

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
}  // namespace

AgentToolRuntime::AgentToolRuntime(Dependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

void AgentToolRuntime::setTools(QList<agent::ToolDefinition> tools)
{
    tools_ = std::move(tools);
}

bool AgentToolRuntime::isReady() const
{
    return dependencies_.validate && dependencies_.policy && dependencies_.call;
}

std::optional<AgentToolRuntime::ToolDescriptor> AgentToolRuntime::inspect(
    const QString& qualifiedName) const
{
    const auto tool =
        std::find_if(tools_.cbegin(), tools_.cend(),
                     [&qualifiedName](const agent::ToolDefinition& candidate)
                     { return candidate.qualifiedName == qualifiedName; });
    if (tool == tools_.cend()) return std::nullopt;
    return ToolDescriptor{*tool, operationKind(*tool)};
}

agent::ToolValidationResult AgentToolRuntime::validate(
    const agent::Action& action) const
{
    if (!dependencies_.validate) return {};
    return dependencies_.validate(action.toolName, action.arguments);
}

infrastructure::mcp::ToolDecision AgentToolRuntime::authorize(
    const agent::Action& action)
{
    pendingApproval_.reset();
    if (!dependencies_.policy) return infrastructure::mcp::ToolDecision::Deny;
    const auto decision = dependencies_.policy(action.toolName);
    if (decision == infrastructure::mcp::ToolDecision::RequireApproval)
        pendingApproval_ = action;
    return decision;
}

const std::optional<agent::Action>& AgentToolRuntime::pendingApproval() const
{
    return pendingApproval_;
}

std::optional<agent::Action> AgentToolRuntime::resolveApproval(bool approved)
{
    auto action = std::exchange(pendingApproval_, std::nullopt);
    if (!approved) action.reset();
    return action;
}

void AgentToolRuntime::clearPendingApproval()
{
    pendingApproval_.reset();
}

QString AgentToolRuntime::callSignature(const agent::Action& action) const
{
    const auto arguments = canonicalJsonValue(action.arguments).toObject();
    return action.toolName + QLatin1Char('\n') +
           QString::fromUtf8(
               QJsonDocument(arguments).toJson(QJsonDocument::Compact));
}

AgentToolRuntime::CallGuardResult AgentToolRuntime::guardCall(
    const agent::Action& action, bool isStatusPoll) const
{
    CallGuardResult result;
    result.signature = callSignature(action);
    if (!lastFailedCallSignature_.isEmpty() &&
        result.signature == lastFailedCallSignature_)
    {
        result.errorMessage = QStringLiteral(
            "The identical tool call already failed. Do not call it again. "
            "Return a final action now, or use meaningfully different "
            "arguments only when the user's request requires another "
            "attempt.");
        return result;
    }
    result.statusPoll =
        isStatusPoll || (!pollableCallSignature_.isEmpty() &&
                         result.signature == pollableCallSignature_);
    if (!result.statusPoll)
        result.errorMessage =
            repeatedCompletedCallError(completedCallHistory_, result.signature);
    return result;
}

void AgentToolRuntime::recordCallResult(const agent::Action& action,
                                        agent::ToolOutcome outcome)
{
    const auto signature = callSignature(action);
    completedCallHistory_.append(signature);
    if (outcome != agent::ToolOutcome::Succeeded &&
        outcome != agent::ToolOutcome::InProgress)
        lastFailedCallSignature_ = signature;
    else
        lastFailedCallSignature_.clear();
}

void AgentToolRuntime::resetCallHistory()
{
    lastFailedCallSignature_.clear();
    completedCallHistory_.clear();
}

AgentToolRuntime::DispatchResult AgentToolRuntime::dispatch(
    const agent::Action& action, qint64 nowMilliseconds)
{
    DispatchResult result;
    const auto signature = callSignature(action);
    result.statusPoll = !pollableCallSignature_.isEmpty() &&
                        signature == pollableCallSignature_;
    if (result.statusPoll && lastPollCompletedAtMs_ >= 0)
    {
        const auto elapsed = nowMilliseconds - lastPollCompletedAtMs_;
        const auto remaining = minimumPollIntervalMilliseconds - elapsed;
        if (remaining > 0)
        {
            pendingPoll_ = action;
            result.status = DispatchResult::Status::Delayed;
            result.delayMilliseconds = static_cast<int>(remaining);
            return result;
        }
    }

    pendingPoll_.reset();
    activeAction_ = action;
    activeCallSignature_ = signature;
    if (dependencies_.call)
        activeRequestId_ =
            dependencies_.call(action.toolName, action.arguments);
    if (activeRequestId_.isEmpty())
    {
        activeAction_.reset();
        activeCallSignature_.clear();
        result.status = DispatchResult::Status::Failed;
        return result;
    }
    result.status = DispatchResult::Status::Started;
    return result;
}

std::optional<AgentToolRuntime::CompletedCall> AgentToolRuntime::completeCall(
    const agent::ToolResult& result, qint64 nowMilliseconds)
{
    if (result.requestId != activeRequestId_ || !activeAction_.has_value())
        return std::nullopt;

    auto normalizedResult = result;
    normalizedResult.outcome = normalizedToolOutcome(result);
    normalizedResult.sideEffectState = normalizedToolSideEffectState(result);
    normalizedResult.isError =
        normalizedResult.outcome != agent::ToolOutcome::Succeeded &&
        normalizedResult.outcome != agent::ToolOutcome::InProgress;

    const auto action = std::exchange(activeAction_, std::nullopt);
    const auto signature = std::exchange(activeCallSignature_, QString{});
    activeRequestId_.clear();
    recordCallResult(*action, normalizedResult.outcome);
    if (normalizedResult.outcome == agent::ToolOutcome::InProgress)
    {
        pollableCallSignature_ = signature;
        lastPollCompletedAtMs_ = nowMilliseconds;
    }
    else
    {
        pollableCallSignature_.clear();
        lastPollCompletedAtMs_ = -1;
    }

    auto operation = ToolOperationKind::Unknown;
    const auto descriptor = inspect(action->toolName);
    if (descriptor.has_value()) operation = descriptor->operationKind;
    return CompletedCall{*action, std::move(normalizedResult), operation};
}

std::optional<agent::Action> AgentToolRuntime::takePendingPoll()
{
    return std::exchange(pendingPoll_, std::nullopt);
}

const std::optional<agent::Action>& AgentToolRuntime::activeAction() const
{
    return activeAction_;
}

const std::optional<agent::Action>& AgentToolRuntime::pendingPoll() const
{
    return pendingPoll_;
}

void AgentToolRuntime::cancelActiveCall()
{
    const auto requestId = activeRequestId_;
    clearTransportState();
    if (!requestId.isEmpty() && dependencies_.cancel)
        dependencies_.cancel(requestId);
}

void AgentToolRuntime::clearTransportState()
{
    activeAction_.reset();
    pendingPoll_.reset();
    activeCallSignature_.clear();
    activeRequestId_.clear();
    pollableCallSignature_.clear();
    lastPollCompletedAtMs_ = -1;
}

ToolOperationKind AgentToolRuntime::operationKind(
    const agent::ToolDefinition& tool) const
{
    const auto readOnlyHint =
        tool.annotations.value(QStringLiteral("readOnlyHint"));
    if (readOnlyHint.isBool())
        return readOnlyHint.toBool() ? ToolOperationKind::ReadOnly
                                     : ToolOperationKind::Mutation;
    if (tool.annotations.value(QStringLiteral("destructiveHint")).toBool())
        return ToolOperationKind::Mutation;
    if (!dependencies_.risk) return ToolOperationKind::Unknown;
    switch (dependencies_.risk(tool.qualifiedName))
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
}  // namespace qtllm::application
