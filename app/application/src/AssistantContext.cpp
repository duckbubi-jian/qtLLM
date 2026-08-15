#include "AssistantContext.hpp"

#include <QJsonDocument>
#include <QJsonObject>

namespace qtllm::application
{
QString assistantContextJson(const AssistantContext& context)
{
    QJsonObject object;
    const auto modelName = context.modelName.trimmed();
    const auto workspaceRoot = context.workspaceRoot.trimmed();
    if (!modelName.isEmpty()) object.insert(QStringLiteral("model"), modelName);
    if (!workspaceRoot.isEmpty())
        object.insert(QStringLiteral("workspaceRoot"), workspaceRoot);
    return object.isEmpty() ? QString{}
                            : QString::fromUtf8(QJsonDocument(object).toJson(
                                  QJsonDocument::Compact));
}
}  // namespace qtllm::application
