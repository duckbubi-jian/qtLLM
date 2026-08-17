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
    QJsonObject outputSchema;
    QJsonObject annotations;
    bool hasOutputSchema = false;
};
}  // namespace qtllm::agent

Q_DECLARE_METATYPE(qtllm::agent::ToolDefinition)
