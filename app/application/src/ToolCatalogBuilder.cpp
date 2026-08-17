#include "ToolCatalogBuilder.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace qtllm::application
{
namespace
{
struct CatalogEntry
{
    QString qualifiedName;
    QByteArray stableKey;
    QJsonObject definition;
};

QJsonObject toolJson(const agent::ToolDefinition& tool)
{
    return {{QStringLiteral("name"), tool.qualifiedName},
            {QStringLiteral("description"), tool.description},
            {QStringLiteral("inputSchema"), tool.inputSchema}};
}

QByteArray serialize(const QJsonArray& definitions)
{
    return QJsonDocument(definitions).toJson(QJsonDocument::Compact);
}
}  // namespace

ToolCatalog ToolCatalogBuilder::build(const QList<agent::ToolDefinition>& tools,
                                      qsizetype maximumBytes)
{
    QList<CatalogEntry> candidates;
    candidates.reserve(tools.size());
    for (const auto& tool : tools)
    {
        auto definition = toolJson(tool);
        candidates.append(
            {tool.qualifiedName,
             QJsonDocument(definition).toJson(QJsonDocument::Compact),
             std::move(definition)});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CatalogEntry& left, const CatalogEntry& right)
              {
                  const auto nameOrder =
                      QString::compare(left.qualifiedName, right.qualifiedName,
                                       Qt::CaseSensitive);
                  return nameOrder == 0 ? left.stableKey < right.stableKey
                                        : nameOrder < 0;
              });

    ToolCatalog result;
    QJsonArray included;
    const auto effectiveMaximum = std::max<qsizetype>(2, maximumBytes);
    for (const auto& candidate : candidates)
    {
        auto proposed = included;
        proposed.append(candidate.definition);
        auto proposedJson = serialize(proposed);
        if (proposedJson.size() > effectiveMaximum)
        {
            ++result.omittedToolCount;
            continue;
        }
        included = std::move(proposed);
        result.json = std::move(proposedJson);
        ++result.includedToolCount;
    }
    return result;
}
}  // namespace qtllm::application
