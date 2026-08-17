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
};
}  // namespace qtllm::agent

Q_DECLARE_METATYPE(qtllm::agent::ToolFailureKind)
Q_DECLARE_METATYPE(qtllm::agent::ToolResult)
