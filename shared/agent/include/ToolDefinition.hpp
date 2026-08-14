#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace qtllm::agent
{
struct ToolDefinition
{
    QString qualifiedName;
    QString serverId;
    QString name;
    QString description;
    QJsonObject inputSchema;
};
}  // namespace qtllm::agent

Q_DECLARE_METATYPE(qtllm::agent::ToolDefinition)
