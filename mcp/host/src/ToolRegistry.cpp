#include "ToolRegistry.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <cmath>

namespace qtllm::infrastructure::mcp
{
namespace
{
constexpr auto maximumValidationDepth = 64;

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

bool matchesType(const QJsonValue& value, const QJsonValue& typeValue)
{
    if (typeValue.isString()) return matchesType(value, typeValue.toString());
    if (!typeValue.isArray()) return true;
    for (const auto& type : typeValue.toArray())
        if (type.isString() && matchesType(value, type.toString())) return true;
    return false;
}

QString typeDescription(const QJsonValue& typeValue)
{
    if (typeValue.isString()) return typeValue.toString();
    if (!typeValue.isArray()) return QStringLiteral("a supported type");
    QStringList types;
    for (const auto& type : typeValue.toArray())
        if (type.isString()) types.append(type.toString());
    return types.join(QStringLiteral(" or "));
}

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

QJsonObject dereferencedSchema(QJsonObject schema, const QJsonObject& root)
{
    for (auto depth = 0; depth < maximumValidationDepth; ++depth)
    {
        const auto ref = schema.value(QStringLiteral("$ref")).toString();
        if (ref.isEmpty()) break;
        const auto resolved = resolveLocalReference(root, ref);
        if (!resolved.isObject()) break;
        schema = resolved.toObject();
    }
    return schema;
}

QJsonArray allowedValues(const QJsonObject& propertySchema,
                         const QJsonObject& root)
{
    const auto schema = dereferencedSchema(propertySchema, root);
    QJsonArray result;
    if (schema.contains(QStringLiteral("const")))
        result.append(schema.value(QStringLiteral("const")));
    const auto enumValue = schema.value(QStringLiteral("enum"));
    if (enumValue.isArray())
        for (const auto& value : enumValue.toArray())
            if (!result.contains(value)) result.append(value);
    return result;
}

QString compactJson(const QJsonArray& value)
{
    return QString::fromUtf8(
               QJsonDocument(value).toJson(QJsonDocument::Compact))
        .left(1'024);
}

QString compactJson(const QJsonValue& value)
{
    const auto wrapped = compactJson(QJsonArray{value});
    return wrapped.sliced(1).chopped(1);
}

QString discriminatorError(const QJsonValue& value, const QJsonObject& schema,
                           const QJsonArray& branches, const QJsonObject& root,
                           const QString& path)
{
    if (!value.isObject() || branches.isEmpty()) return {};
    const auto object = value.toObject();
    QStringList candidates;
    const auto explicitProperty = schema.value(QStringLiteral("discriminator"))
                                      .toObject()
                                      .value(QStringLiteral("propertyName"))
                                      .toString();
    if (!explicitProperty.isEmpty()) candidates.append(explicitProperty);

    const auto firstBranch = branches.first();
    if (firstBranch.isObject())
    {
        const auto firstSchema =
            dereferencedSchema(firstBranch.toObject(), root);
        for (const auto& name :
             firstSchema.value(QStringLiteral("properties")).toObject().keys())
            if (!candidates.contains(name)) candidates.append(name);
    }

    for (const auto& candidate : candidates)
    {
        QJsonArray allowed;
        auto presentInEveryBranch = true;
        auto requiredInEveryBranch = true;
        for (const auto& branch : branches)
        {
            if (!branch.isObject())
            {
                presentInEveryBranch = false;
                break;
            }
            const auto branchSchema =
                dereferencedSchema(branch.toObject(), root);
            const auto property =
                branchSchema.value(QStringLiteral("properties"))
                    .toObject()
                    .value(candidate);
            if (!property.isObject())
            {
                presentInEveryBranch = false;
                break;
            }
            const auto branchValues = allowedValues(property.toObject(), root);
            if (branchValues.isEmpty())
            {
                presentInEveryBranch = false;
                break;
            }
            for (const auto& branchValue : branchValues)
                if (!allowed.contains(branchValue)) allowed.append(branchValue);
            if (!branchSchema.value(QStringLiteral("required"))
                     .toArray()
                     .contains(candidate))
                requiredInEveryBranch = false;
        }
        if (!presentInEveryBranch || allowed.isEmpty()) continue;
        const auto candidatePath = path + QLatin1Char('.') + candidate;
        if (!object.contains(candidate))
        {
            if (requiredInEveryBranch)
                return QStringLiteral("%1 is required and must be one of %2.")
                    .arg(candidatePath, compactJson(allowed));
            continue;
        }
        if (!allowed.contains(object.value(candidate)))
            return QStringLiteral("%1 must be one of %2.")
                .arg(candidatePath, compactJson(allowed));
    }
    return {};
}

bool validateValue(const QJsonValue& value, const QJsonObject& schema,
                   const QJsonObject& root, const QString& path,
                   QString& errorMessage, int depth = 0)
{
    if (depth > maximumValidationDepth)
    {
        errorMessage =
            QStringLiteral("%1 exceeds the supported schema depth.").arg(path);
        return false;
    }

    const auto reference = schema.value(QStringLiteral("$ref")).toString();
    if (reference.startsWith(QLatin1String("#/")) ||
        reference == QLatin1String("#"))
    {
        const auto resolved = resolveLocalReference(root, reference);
        if (!resolved.isObject())
        {
            errorMessage = QStringLiteral("%1 uses unresolved schema ref %2.")
                               .arg(path, reference);
            return false;
        }
        if (!validateValue(value, resolved.toObject(), root, path, errorMessage,
                           depth + 1))
            return false;
    }

    const auto allOf = schema.value(QStringLiteral("allOf"));
    if (allOf.isArray())
    {
        for (const auto& branch : allOf.toArray())
        {
            if (!branch.isObject()) continue;
            if (!validateValue(value, branch.toObject(), root, path,
                               errorMessage, depth + 1))
                return false;
        }
    }

    const auto anyOf = schema.value(QStringLiteral("anyOf"));
    if (anyOf.isArray() && !anyOf.toArray().isEmpty())
    {
        QString firstBranchError;
        auto matched = false;
        for (const auto& branch : anyOf.toArray())
        {
            if (!branch.isObject()) continue;
            QString branchError;
            if (validateValue(value, branch.toObject(), root, path, branchError,
                              depth + 1))
            {
                matched = true;
                break;
            }
            if (firstBranchError.isEmpty()) firstBranchError = branchError;
        }
        if (!matched)
        {
            errorMessage = QStringLiteral(
                               "%1 does not match any allowed "
                               "schema. %2")
                               .arg(path, firstBranchError);
            return false;
        }
    }

    const auto oneOf = schema.value(QStringLiteral("oneOf"));
    if (oneOf.isArray() && !oneOf.toArray().isEmpty())
    {
        QString firstBranchError;
        auto matches = 0;
        for (const auto& branch : oneOf.toArray())
        {
            if (!branch.isObject()) continue;
            QString branchError;
            if (validateValue(value, branch.toObject(), root, path, branchError,
                              depth + 1))
                ++matches;
            else if (firstBranchError.isEmpty())
                firstBranchError = branchError;
        }
        if (matches == 0)
        {
            errorMessage =
                discriminatorError(value, schema, oneOf.toArray(), root, path);
            if (errorMessage.isEmpty())
                errorMessage =
                    QStringLiteral("%1 does not match any oneOf branch. %2")
                        .arg(path, firstBranchError);
            return false;
        }
        if (matches > 1)
        {
            errorMessage =
                QStringLiteral("%1 matches more than one oneOf branch.")
                    .arg(path);
            return false;
        }
    }

    const auto typeValue = schema.value(QStringLiteral("type"));
    if ((typeValue.isString() || typeValue.isArray()) &&
        !matchesType(value, typeValue))
    {
        errorMessage = QStringLiteral("%1 must be %2.")
                           .arg(path, typeDescription(typeValue));
        return false;
    }

    if (schema.contains(QStringLiteral("const")) &&
        schema.value(QStringLiteral("const")) != value)
    {
        errorMessage =
            QStringLiteral("%1 must equal %2.")
                .arg(path, compactJson(schema.value(QStringLiteral("const"))));
        return false;
    }

    const auto enumValue = schema.value(QStringLiteral("enum"));
    if (enumValue.isArray() && !enumValue.toArray().contains(value))
    {
        errorMessage = QStringLiteral("%1 must be one of %2.")
                           .arg(path, compactJson(enumValue.toArray()));
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
            if (!validateValue(object.value(property.key()),
                               property->toObject(), root,
                               path + QLatin1Char('.') + property.key(),
                               errorMessage, depth + 1))
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
                        array.at(index), itemSchema.toObject(), root,
                        QStringLiteral("%1[%2]").arg(path).arg(index),
                        errorMessage, depth + 1))
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
                         definition->inputSchema, QStringLiteral("arguments"),
                         errorMessage);
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
                         definition->outputSchema,
                         QStringLiteral("structuredContent"), errorMessage);
}
}  // namespace qtllm::infrastructure::mcp
