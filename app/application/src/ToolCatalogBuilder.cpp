#include "ToolCatalogBuilder.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

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
    QJsonObject compactDefinition;
};

constexpr auto maximumSchemaSummaryDepth = 8;
constexpr auto maximumSchemaSummaryBranches = 16;
constexpr auto maximumSchemaSummaryProperties = 48;
constexpr auto maximumPurposeCharacters = 240;

QString decodedPointerToken(QString token)
{
    return token.replace(QStringLiteral("~1"), QStringLiteral("/"))
        .replace(QStringLiteral("~0"), QStringLiteral("~"));
}

QJsonValue resolveLocalReference(const QJsonObject& root, const QString& ref)
{
    if (ref == QLatin1String("#")) return root;
    if (!ref.startsWith(QLatin1String("#/"))) return {};

    QJsonValue current(root);
    const auto tokens = ref.sliced(2).split(QLatin1Char('/'));
    for (const auto& encodedToken : tokens)
    {
        const auto token = decodedPointerToken(encodedToken);
        if (current.isObject())
            current = current.toObject().value(token);
        else if (current.isArray())
        {
            bool ok = false;
            const auto index = token.toInt(&ok);
            const auto values = current.toArray();
            if (!ok || index < 0 || index >= values.size()) return {};
            current = values.at(index);
        }
        else
            return {};
        if (current.isUndefined()) return {};
    }
    return current;
}

QJsonObject schemaSummary(const QJsonObject& schema, const QJsonObject& root,
                          int depth, QSet<QString> activeReferences);

QJsonArray branchSummary(const QJsonValue& value, const QJsonObject& root,
                         int depth, const QSet<QString>& activeReferences,
                         int& omitted)
{
    QJsonArray result;
    omitted = 0;
    if (!value.isArray()) return result;
    const auto branches = value.toArray();
    const auto included =
        std::min<qsizetype>(branches.size(), maximumSchemaSummaryBranches);
    for (qsizetype index = 0; index < included; ++index)
    {
        if (!branches.at(index).isObject()) continue;
        result.append(schemaSummary(branches.at(index).toObject(), root,
                                    depth + 1, activeReferences));
    }
    omitted = static_cast<int>(branches.size() - included);
    return result;
}

QJsonObject schemaSummary(const QJsonObject& schema, const QJsonObject& root,
                          int depth, QSet<QString> activeReferences)
{
    QJsonObject result;
    const auto reference = schema.value(QStringLiteral("$ref")).toString();
    if (!reference.isEmpty())
    {
        const auto resolved = resolveLocalReference(root, reference);
        if (resolved.isObject() && !activeReferences.contains(reference))
        {
            activeReferences.insert(reference);
            result = schemaSummary(resolved.toObject(), root, depth,
                                   activeReferences);
        }
        else
            result.insert(QStringLiteral("ref"), reference);
    }

    for (const auto& key : {QStringLiteral("type"), QStringLiteral("const"),
                            QStringLiteral("enum"), QStringLiteral("required"),
                            QStringLiteral("discriminator")})
    {
        if (schema.contains(key)) result.insert(key, schema.value(key));
    }
    if (schema.value(QStringLiteral("additionalProperties")).isBool() &&
        !schema.value(QStringLiteral("additionalProperties")).toBool())
        result.insert(QStringLiteral("additionalProperties"), false);

    if (depth >= maximumSchemaSummaryDepth) return result;

    const auto properties = schema.value(QStringLiteral("properties"));
    if (properties.isObject())
    {
        QJsonObject summarizedProperties;
        const auto names = properties.toObject().keys();
        const auto included =
            std::min<qsizetype>(names.size(), maximumSchemaSummaryProperties);
        for (qsizetype index = 0; index < included; ++index)
        {
            const auto property = properties.toObject().value(names.at(index));
            if (property.isObject())
                summarizedProperties.insert(
                    names.at(index),
                    schemaSummary(property.toObject(), root, depth + 1,
                                  activeReferences));
        }
        result.insert(QStringLiteral("properties"), summarizedProperties);
        if (names.size() > included)
            result.insert(QStringLiteral("omittedProperties"),
                          names.size() - included);
    }

    const auto items = schema.value(QStringLiteral("items"));
    if (items.isObject())
        result.insert(
            QStringLiteral("items"),
            schemaSummary(items.toObject(), root, depth + 1, activeReferences));

    for (const auto& key : {QStringLiteral("oneOf"), QStringLiteral("anyOf"),
                            QStringLiteral("allOf")})
    {
        int omitted = 0;
        const auto branches = branchSummary(schema.value(key), root, depth,
                                            activeReferences, omitted);
        if (!branches.isEmpty()) result.insert(key, branches);
        if (omitted > 0)
            result.insert(key + QStringLiteral("Omitted"), omitted);
    }
    return result;
}

QString compactPurpose(const QString& description)
{
    return description.simplified().left(maximumPurposeCharacters);
}

QJsonObject minimalIndexEntry(const agent::ToolDefinition& tool)
{
    QJsonObject result{{QStringLiteral("name"), tool.qualifiedName}};
    const auto purpose = compactPurpose(tool.description);
    if (!purpose.isEmpty()) result.insert(QStringLiteral("purpose"), purpose);
    const auto required = tool.inputSchema.value(QStringLiteral("required"));
    if (required.isArray()) result.insert(QStringLiteral("required"), required);
    return result;
}

QJsonObject toolJson(const agent::ToolDefinition& tool)
{
    QJsonObject result{{QStringLiteral("name"), tool.qualifiedName},
                       {QStringLiteral("description"), tool.description},
                       {QStringLiteral("inputSchema"), tool.inputSchema}};
    if (tool.hasOutputSchema || !tool.outputSchema.isEmpty())
        result.insert(QStringLiteral("outputSchema"), tool.outputSchema);
    if (!tool.annotations.isEmpty())
        result.insert(QStringLiteral("annotations"), tool.annotations);
    return result;
}

QByteArray serialize(const QJsonArray& definitions)
{
    return QJsonDocument(definitions).toJson(QJsonDocument::Compact);
}
}  // namespace

QJsonObject ToolCatalogBuilder::compactDefinition(
    const agent::ToolDefinition& tool)
{
    auto result = minimalIndexEntry(tool);
    result.insert(QStringLiteral("arguments"),
                  schemaSummary(tool.inputSchema, tool.inputSchema, 0, {}));
    if (tool.annotations.value(QStringLiteral("readOnlyHint")).isBool())
        result.insert(QStringLiteral("readOnly"),
                      tool.annotations.value(QStringLiteral("readOnlyHint")));
    return result;
}

ToolCatalog ToolCatalogBuilder::build(const QList<agent::ToolDefinition>& tools,
                                      qsizetype maximumBytes,
                                      qsizetype maximumIndexBytes)
{
    QList<CatalogEntry> candidates;
    candidates.reserve(tools.size());
    for (const auto& tool : tools)
    {
        auto definition = toolJson(tool);
        candidates.append(
            {tool.qualifiedName,
             QJsonDocument(definition).toJson(QJsonDocument::Compact),
             std::move(definition), compactDefinition(tool)});
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
            result.omittedToolNames.append(candidate.qualifiedName);
            continue;
        }
        included = std::move(proposed);
        result.json = std::move(proposedJson);
        ++result.includedToolCount;
    }

    QJsonArray indexed;
    const auto effectiveIndexMaximum =
        std::max<qsizetype>(2, maximumIndexBytes);
    for (const auto& candidate : candidates)
    {
        auto proposed = indexed;
        proposed.append(candidate.compactDefinition);
        auto proposedJson = serialize(proposed);
        if (proposedJson.size() > effectiveIndexMaximum)
        {
            auto minimal = proposed;
            minimal.removeAt(minimal.size() - 1);
            minimal.append(
                QJsonObject{{QStringLiteral("name"), candidate.qualifiedName}});
            proposedJson = serialize(minimal);
            if (proposedJson.size() > effectiveIndexMaximum)
            {
                ++result.unindexedToolCount;
                result.unindexedToolNames.append(candidate.qualifiedName);
                continue;
            }
            proposed = std::move(minimal);
        }
        indexed = std::move(proposed);
        result.indexJson = std::move(proposedJson);
        ++result.indexedToolCount;
    }
    return result;
}
}  // namespace qtllm::application
