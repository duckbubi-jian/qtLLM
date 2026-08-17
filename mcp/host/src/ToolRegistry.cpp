#include "ToolRegistry.hpp"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>

namespace qtllm::infrastructure::mcp
{
namespace
{
bool matchesType(const QJsonValue& value, const QString& type)
{
    if (type == QStringLiteral("object")) return value.isObject();
    if (type == QStringLiteral("array")) return value.isArray();
    if (type == QStringLiteral("string")) return value.isString();
    if (type == QStringLiteral("number")) return value.isDouble();
    if (type == QStringLiteral("integer"))
        return value.isDouble() && std::isfinite(value.toDouble()) &&
               std::floor(value.toDouble()) == value.toDouble();
    if (type == QStringLiteral("boolean")) return value.isBool();
    if (type == QStringLiteral("null")) return value.isNull();
    return false;
}

bool validateValue(const QJsonValue& value, const QJsonObject& schema,
                   const QString& path, QString& errorMessage)
{
    const auto typeValue = schema.value(QStringLiteral("type"));
    if (typeValue.isString() && !matchesType(value, typeValue.toString()))
    {
        errorMessage =
            QStringLiteral("%1 must be %2.").arg(path, typeValue.toString());
        return false;
    }

    const auto enumValue = schema.value(QStringLiteral("enum"));
    if (enumValue.isArray() && !enumValue.toArray().contains(value))
    {
        errorMessage = QStringLiteral("%1 is not an allowed value.").arg(path);
        return false;
    }

    if (value.isObject())
    {
        const auto object = value.toObject();
        const auto required = schema.value(QStringLiteral("required"));
        if (required.isArray())
        {
            for (const auto& item : required.toArray())
            {
                if (!item.isString()) continue;
                if (!object.contains(item.toString()))
                {
                    errorMessage = QStringLiteral("%1.%2 is required.")
                                       .arg(path, item.toString());
                    return false;
                }
            }
        }

        const auto properties =
            schema.value(QStringLiteral("properties")).toObject();
        if (schema.value(QStringLiteral("additionalProperties")).isBool() &&
            !schema.value(QStringLiteral("additionalProperties")).toBool())
        {
            for (auto item = object.constBegin(); item != object.constEnd();
                 ++item)
            {
                if (!properties.contains(item.key()))
                {
                    errorMessage = QStringLiteral("%1.%2 is not allowed.")
                                       .arg(path, item.key());
                    return false;
                }
            }
        }
        for (auto property = properties.constBegin();
             property != properties.constEnd(); ++property)
        {
            if (!object.contains(property.key()) || !property->isObject())
                continue;
            if (!validateValue(
                    object.value(property.key()), property->toObject(),
                    path + QLatin1Char('.') + property.key(), errorMessage))
                return false;
        }
    }
    else if (value.isArray())
    {
        const auto array = value.toArray();
        const auto minimum = schema.value(QStringLiteral("minItems"));
        const auto maximum = schema.value(QStringLiteral("maxItems"));
        if (minimum.isDouble() && array.size() < minimum.toInteger())
        {
            errorMessage = QStringLiteral("%1 has too few items.").arg(path);
            return false;
        }
        if (maximum.isDouble() && array.size() > maximum.toInteger())
        {
            errorMessage = QStringLiteral("%1 has too many items.").arg(path);
            return false;
        }
        const auto itemSchema = schema.value(QStringLiteral("items"));
        if (itemSchema.isObject())
        {
            for (qsizetype index = 0; index < array.size(); ++index)
            {
                if (!validateValue(
                        array.at(index), itemSchema.toObject(),
                        QStringLiteral("%1[%2]").arg(path).arg(index),
                        errorMessage))
                    return false;
            }
        }
    }
    else if (value.isString())
    {
        const auto length = value.toString().size();
        const auto minimum = schema.value(QStringLiteral("minLength"));
        const auto maximum = schema.value(QStringLiteral("maxLength"));
        if (minimum.isDouble() && length < minimum.toInteger())
        {
            errorMessage = QStringLiteral("%1 is too short.").arg(path);
            return false;
        }
        if (maximum.isDouble() && length > maximum.toInteger())
        {
            errorMessage = QStringLiteral("%1 is too long.").arg(path);
            return false;
        }
    }
    else if (value.isDouble())
    {
        const auto number = value.toDouble();
        const auto minimum = schema.value(QStringLiteral("minimum"));
        const auto maximum = schema.value(QStringLiteral("maximum"));
        if (minimum.isDouble() && number < minimum.toDouble())
        {
            errorMessage = QStringLiteral("%1 is below the minimum.").arg(path);
            return false;
        }
        if (maximum.isDouble() && number > maximum.toDouble())
        {
            errorMessage = QStringLiteral("%1 exceeds the maximum.").arg(path);
            return false;
        }
    }
    return true;
}
}  // namespace

bool ToolRegistry::replaceServerTools(const QString& serverId,
                                      const QList<agent::ToolDefinition>& tools,
                                      QString& errorMessage)
{
    if (serverId.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Tool serverId must not be empty.");
        return false;
    }
    QHash<QString, agent::ToolDefinition> replacement;
    for (auto definition : tools)
    {
        if (definition.serverId != serverId || definition.name.isEmpty())
        {
            errorMessage =
                QStringLiteral("Invalid tool definition for %1.").arg(serverId);
            return false;
        }
        definition.qualifiedName =
            serverId + QLatin1Char('.') + definition.name;
        if (replacement.contains(definition.qualifiedName))
        {
            errorMessage = QStringLiteral("Duplicate tool: %1")
                               .arg(definition.qualifiedName);
            return false;
        }
        replacement.insert(definition.qualifiedName, definition);
    }

    removeServer(serverId);
    for (auto item = replacement.constBegin(); item != replacement.constEnd();
         ++item)
        tools_.insert(item.key(), item.value());
    return true;
}

void ToolRegistry::removeServer(const QString& serverId)
{
    const auto prefix = serverId + QLatin1Char('.');
    for (auto iterator = tools_.begin(); iterator != tools_.end();)
    {
        if (iterator.key().startsWith(prefix))
            iterator = tools_.erase(iterator);
        else
            ++iterator;
    }
}

void ToolRegistry::clear()
{
    tools_.clear();
}

QList<agent::ToolDefinition> ToolRegistry::tools() const
{
    return tools_.values();
}

const agent::ToolDefinition* ToolRegistry::find(
    const QString& qualifiedName) const
{
    const auto iterator = tools_.constFind(qualifiedName);
    return iterator == tools_.constEnd() ? nullptr : &iterator.value();
}

bool ToolRegistry::validateArguments(const QString& qualifiedName,
                                     const QJsonObject& arguments,
                                     QString& errorMessage) const
{
    const auto* definition = find(qualifiedName);
    if (definition == nullptr)
    {
        errorMessage =
            QStringLiteral("Tool is not registered: %1").arg(qualifiedName);
        return false;
    }
    return validateValue(arguments, definition->inputSchema,
                         QStringLiteral("arguments"), errorMessage);
}

bool ToolRegistry::validateOutput(const QString& qualifiedName,
                                  const QJsonValue& output,
                                  QString& errorMessage) const
{
    const auto* definition = find(qualifiedName);
    if (definition == nullptr)
    {
        errorMessage =
            QStringLiteral("Tool is not registered: %1").arg(qualifiedName);
        return false;
    }
    if (!definition->hasOutputSchema && definition->outputSchema.isEmpty())
        return true;
    if (output.isUndefined())
    {
        errorMessage = QStringLiteral(
            "Tool result did not include structuredContent required by "
            "outputSchema.");
        return false;
    }
    return validateValue(output, definition->outputSchema,
                         QStringLiteral("structuredContent"), errorMessage);
}
}  // namespace qtllm::infrastructure::mcp
