#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace qtllm::agent
{
enum class ToolFailureKind
{
    None,
    LocalValidation,
    Authorization,
    Transport,
    Protocol,
    Server,
    Tool
};

// Controller-facing result categories.  ToolFailureKind preserves the
// transport's diagnostic classification; ToolOutcome is the normalized
// lifecycle result consumed by the agent loop.
enum class ToolOutcome
{
    Succeeded,
    InProgress,
    ValidationFailed,
    Denied,
    TransportFailed,
    ProtocolFailed,
    ServerFailed,
    ToolFailed,
    Cancelled
};

// This is deliberately conservative: after a request has left the process,
// a failure may not tell us whether a remote side effect happened.
enum class ToolSideEffectState
{
    NotDispatched,
    Dispatched,
    Succeeded,
    KnownFailed,
    Uncertain
};

struct ToolResult
{
    QString requestId;
    QString serverId;
    QString toolName;
    bool isError = false;
    bool truncated = false;
    qsizetype originalBytes = 0;
    QJsonObject result;
    QJsonValue structuredContent;
    QJsonArray contentBlocks;
    QStringList unknownContentBlockTypes;
    QString errorCode;
    QString errorMessage;
    ToolFailureKind failureKind = ToolFailureKind::None;
    ToolOutcome outcome = ToolOutcome::Succeeded;
    ToolSideEffectState sideEffectState = ToolSideEffectState::NotDispatched;
};
}  // namespace qtllm::agent

Q_DECLARE_METATYPE(qtllm::agent::ToolFailureKind)
Q_DECLARE_METATYPE(qtllm::agent::ToolOutcome)
Q_DECLARE_METATYPE(qtllm::agent::ToolSideEffectState)
Q_DECLARE_METATYPE(qtllm::agent::ToolResult)
