#include "ToolResultStatus.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace qtllm::application
{
namespace
{
enum class ProgressSignal
{
    None,
    InProgress,
    Succeeded,
    Failed
};

constexpr auto maximumTraversalDepth = 32;

ProgressSignal mergeSignal(ProgressSignal left, ProgressSignal right)
{
    if (left == ProgressSignal::Failed || right == ProgressSignal::Failed)
        return ProgressSignal::Failed;
    if (left == ProgressSignal::InProgress ||
        right == ProgressSignal::InProgress)
        return ProgressSignal::InProgress;
    if (left == ProgressSignal::Succeeded || right == ProgressSignal::Succeeded)
        return ProgressSignal::Succeeded;
    return ProgressSignal::None;
}

ProgressSignal stateStringSignal(const QString& value)
{
    const auto state = value.trimmed().toLower();
    static const QSet<QString> inProgressStates{
        QStringLiteral("running"),     QStringLiteral("pending"),
        QStringLiteral("queued"),      QStringLiteral("in_progress"),
        QStringLiteral("in-progress"), QStringLiteral("in progress"),
        QStringLiteral("processing")};
    if (inProgressStates.contains(state)) return ProgressSignal::InProgress;

    static const QSet<QString> succeededStates{
        QStringLiteral("complete"), QStringLiteral("completed"),
        QStringLiteral("success"),  QStringLiteral("succeeded"),
        QStringLiteral("finished"), QStringLiteral("done")};
    if (succeededStates.contains(state)) return ProgressSignal::Succeeded;

    static const QSet<QString> failedStates{
        QStringLiteral("failed"),    QStringLiteral("error"),
        QStringLiteral("cancelled"), QStringLiteral("canceled"),
        QStringLiteral("aborted"),   QStringLiteral("rejected"),
        QStringLiteral("stopped")};
    return failedStates.contains(state) ? ProgressSignal::Failed
                                        : ProgressSignal::None;
}

ProgressSignal localProgressSignal(const QJsonObject& object)
{
    auto signal = ProgressSignal::None;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        const auto key = it.key().toLower();
        if (key == QStringLiteral("isrunning") && it.value().isBool())
            signal = mergeSignal(signal, it.value().toBool()
                                             ? ProgressSignal::InProgress
                                             : ProgressSignal::Succeeded);
        else if ((key == QStringLiteral("status") ||
                  key == QStringLiteral("state")) &&
                 it.value().isString())
            signal =
                mergeSignal(signal, stateStringSignal(it.value().toString()));
        else if ((key == QStringLiteral("ok") ||
                  key == QStringLiteral("success") ||
                  key == QStringLiteral("succeeded")) &&
                 it.value().isBool() && !it.value().toBool())
            signal = mergeSignal(signal, ProgressSignal::Failed);
    }
    return signal;
}

ProgressSignal progressSignal(const QJsonValue& value, int depth)
{
    if (depth > maximumTraversalDepth) return ProgressSignal::None;
    if (value.isArray())
    {
        auto signal = ProgressSignal::None;
        for (const auto& item : value.toArray())
            signal = mergeSignal(signal, progressSignal(item, depth + 1));
        return signal;
    }
    if (!value.isObject()) return ProgressSignal::None;

    const auto object = value.toObject();
    const auto local = localProgressSignal(object);
    if (local != ProgressSignal::None) return local;

    auto signal = ProgressSignal::None;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        signal = mergeSignal(signal, progressSignal(it.value(), depth + 1));
    return signal;
}

ProgressSignal resultProgressSignal(const agent::ToolResult& result)
{
    auto payload = result.structuredContent;
    if (payload.isUndefined() || payload.isNull())
    {
        payload = result.result.value(QStringLiteral("structuredContent"));
        if (payload.isUndefined() || payload.isNull()) payload = result.result;
    }
    return progressSignal(payload, 0);
}
}  // namespace

agent::ToolOutcome normalizedToolOutcome(const agent::ToolResult& result)
{
    if (result.outcome != agent::ToolOutcome::Succeeded) return result.outcome;
    if (!result.isError)
    {
        const auto signal = resultProgressSignal(result);
        if (signal == ProgressSignal::InProgress)
            return agent::ToolOutcome::InProgress;
        if (signal == ProgressSignal::Failed)
            return agent::ToolOutcome::ToolFailed;
        return agent::ToolOutcome::Succeeded;
    }

    switch (result.failureKind)
    {
        case agent::ToolFailureKind::LocalValidation:
            return agent::ToolOutcome::ValidationFailed;
        case agent::ToolFailureKind::Authorization:
            return agent::ToolOutcome::Denied;
        case agent::ToolFailureKind::Transport:
            return result.errorCode == QLatin1String("cancelled") ||
                           result.errorCode == QLatin1String("canceled")
                       ? agent::ToolOutcome::Cancelled
                       : agent::ToolOutcome::TransportFailed;
        case agent::ToolFailureKind::Protocol:
            return agent::ToolOutcome::ProtocolFailed;
        case agent::ToolFailureKind::Server:
            return agent::ToolOutcome::ServerFailed;
        case agent::ToolFailureKind::Tool:
        case agent::ToolFailureKind::None:
            return agent::ToolOutcome::ToolFailed;
    }
    return agent::ToolOutcome::ToolFailed;
}

agent::ToolSideEffectState normalizedToolSideEffectState(
    const agent::ToolResult& result)
{
    const auto outcome = normalizedToolOutcome(result);
    if (outcome == agent::ToolOutcome::InProgress)
        return result.requestId.isEmpty()
                   ? agent::ToolSideEffectState::NotDispatched
                   : agent::ToolSideEffectState::Dispatched;
    if (outcome == agent::ToolOutcome::ToolFailed)
        return agent::ToolSideEffectState::KnownFailed;
    if (result.sideEffectState != agent::ToolSideEffectState::NotDispatched)
        return result.sideEffectState;
    if (result.requestId.isEmpty())
        return agent::ToolSideEffectState::NotDispatched;

    switch (outcome)
    {
        case agent::ToolOutcome::Succeeded:
            return agent::ToolSideEffectState::Succeeded;
        case agent::ToolOutcome::InProgress:
            return agent::ToolSideEffectState::Dispatched;
        case agent::ToolOutcome::ValidationFailed:
        case agent::ToolOutcome::Denied:
        case agent::ToolOutcome::ToolFailed:
            return agent::ToolSideEffectState::KnownFailed;
        case agent::ToolOutcome::Cancelled:
        case agent::ToolOutcome::TransportFailed:
        case agent::ToolOutcome::ProtocolFailed:
        case agent::ToolOutcome::ServerFailed:
            return agent::ToolSideEffectState::Uncertain;
    }
    return agent::ToolSideEffectState::Uncertain;
}

QString toolOutcomeName(agent::ToolOutcome outcome)
{
    switch (outcome)
    {
        case agent::ToolOutcome::Succeeded:
            return QStringLiteral("succeeded");
        case agent::ToolOutcome::InProgress:
            return QStringLiteral("in_progress");
        case agent::ToolOutcome::ValidationFailed:
            return QStringLiteral("validation_failed");
        case agent::ToolOutcome::Denied:
            return QStringLiteral("denied");
        case agent::ToolOutcome::TransportFailed:
            return QStringLiteral("transport_failed");
        case agent::ToolOutcome::ProtocolFailed:
            return QStringLiteral("protocol_failed");
        case agent::ToolOutcome::ServerFailed:
            return QStringLiteral("server_failed");
        case agent::ToolOutcome::ToolFailed:
            return QStringLiteral("tool_failed");
        case agent::ToolOutcome::Cancelled:
            return QStringLiteral("cancelled");
    }
    return QStringLiteral("tool_failed");
}

QString toolSideEffectStateName(agent::ToolSideEffectState state)
{
    switch (state)
    {
        case agent::ToolSideEffectState::NotDispatched:
            return QStringLiteral("not_dispatched");
        case agent::ToolSideEffectState::Dispatched:
            return QStringLiteral("dispatched");
        case agent::ToolSideEffectState::Succeeded:
            return QStringLiteral("succeeded");
        case agent::ToolSideEffectState::KnownFailed:
            return QStringLiteral("known_failed");
        case agent::ToolSideEffectState::Uncertain:
            return QStringLiteral("uncertain");
    }
    return QStringLiteral("uncertain");
}

bool toolResultIndicatesInProgress(const agent::ToolResult& result)
{
    return normalizedToolOutcome(result) == agent::ToolOutcome::InProgress;
}
}  // namespace qtllm::application
