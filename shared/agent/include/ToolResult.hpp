#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace qtllm::agent
{
struct ToolResult
{
    QString requestId;
    QString serverId;
    QString toolName;
    bool isError = false;
    bool truncated = false;
    qsizetype originalBytes = 0;
    QJsonObject result;
    QString errorCode;
    QString errorMessage;
};
}  // namespace qtllm::agent

Q_DECLARE_METATYPE(qtllm::agent::ToolResult)
