#pragma once

#include "ToolDefinition.hpp"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace qtllm::infrastructure::mcp
{
class ToolRegistry final
{
   public:
    bool replaceServerTools(const QString& serverId,
                            const QList<agent::ToolDefinition>& tools,
                            QString& errorMessage);
    void removeServer(const QString& serverId);
    void clear();

    [[nodiscard]] QList<agent::ToolDefinition> tools() const;
    [[nodiscard]] const agent::ToolDefinition* find(
        const QString& qualifiedName) const;
    [[nodiscard]] bool validateArguments(const QString& qualifiedName,
                                         const QJsonObject& arguments,
                                         QString& errorMessage) const;

   private:
    QHash<QString, agent::ToolDefinition> tools_;
};
}  // namespace qtllm::infrastructure::mcp
