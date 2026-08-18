#include "ToolRegistry.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <cmath>
#include <utility>

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

QString encodedPointerToken(QString token)
{
    return token.replace(QStringLiteral("~"), QStringLiteral("~0"))
        .replace(QStringLiteral("/"), QStringLiteral("~1"));
}

QString childSchemaPath(const QString& parent, const QString& keyword,
                        const QString& token = {})
{
    auto result = parent + QLatin1Char('/') + keyword;
    if (!token.isEmpty())
        result += QLatin1Char('/') + encodedPointerToken(token);
    return result;
}

void assignIssue(agent::ToolValidationIssue& issue, const QString& instancePath,
                 const QString& schemaPath, const QString& keyword,
                 const QString& message)
{
    issue.instancePath = instancePath;
    issue.schemaPath = schemaPath;
    issue.keyword = keyword;
    issue.message = message;
}

bool assignDiscriminatorIssue(const QJsonValue& value,
                              const QJsonObject& schema,
                              const QJsonArray& branches,
                              const QJsonObject& root, const QString& path,
                              const QString& schemaPath,
                              agent::ToolValidationIssue& issue)
{
    if (!value.isObject() || branches.isEmpty()) return false;
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
            {
                assignIssue(
                    issue, candidatePath,
                    childSchemaPath(schemaPath, QStringLiteral("oneOf")),
                    QStringLiteral("discriminator"),
                    QStringLiteral("%1 is required and must be one of %2.")
                        .arg(candidatePath, compactJson(allowed)));
                return true;
            }
            continue;
        }
        if (!allowed.contains(object.value(candidate)))
        {
            assignIssue(issue, candidatePath,
                        childSchemaPath(schemaPath, QStringLiteral("oneOf")),
                        QStringLiteral("discriminator"),
                        QStringLiteral("%1 must be one of %2.")
                            .arg(candidatePath, compactJson(allowed)));
            return true;
        }
    }
    return false;
}

bool matchesExplicitDiscriminator(const QJsonValue& value,
                                  const QJsonObject& schema,
                                  const QJsonObject& branch,
                                  const QJsonObject& root)
{
    if (!value.isObject()) return false;
    const auto propertyName = schema.value(QStringLiteral("discriminator"))
                                  .toObject()
                                  .value(QStringLiteral("propertyName"))
                                  .toString();
    const auto object = value.toObject();
    if (propertyName.isEmpty() || !object.contains(propertyName)) return false;
    const auto branchSchema = dereferencedSchema(branch, root);
    const auto propertySchema = branchSchema.value(QStringLiteral("properties"))
                                    .toObject()
                                    .value(propertyName)
                                    .toObject();
    return allowedValues(propertySchema, root)
        .contains(object.value(propertyName));
}

bool validateValue(const QJsonValue& value, const QJsonObject& schema,
                   const QJsonObject& root, const QString& path,
                   const QString& schemaPath, agent::ToolValidationIssue& issue,
                   int depth = 0)
{
    if (depth > maximumValidationDepth)
    {
        assignIssue(
            issue, path, schemaPath, QStringLiteral("depth"),
            QStringLiteral("%1 exceeds the supported schema depth.").arg(path));
        return false;
    }

    const auto reference = schema.value(QStringLiteral("$ref")).toString();
    if (reference.startsWith(QLatin1String("#/")) ||
        reference == QLatin1String("#"))
    {
        const auto resolved = resolveLocalReference(root, reference);
        if (!resolved.isObject())
        {
            assignIssue(issue, path,
                        childSchemaPath(schemaPath, QStringLiteral("$ref")),
                        QStringLiteral("$ref"),
                        QStringLiteral("%1 uses unresolved schema ref %2.")
                            .arg(path, reference));
            return false;
        }
        if (!validateValue(value, resolved.toObject(), root, path, reference,
                           issue, depth + 1))
            return false;
    }

    const auto allOf = schema.value(QStringLiteral("allOf"));
    if (allOf.isArray())
    {
        const auto branches = allOf.toArray();
        for (qsizetype index = 0; index < branches.size(); ++index)
        {
            const auto branch = branches.at(index);
            if (!branch.isObject()) continue;
            if (!validateValue(
                    value, branch.toObject(), root, path,
                    childSchemaPath(schemaPath, QStringLiteral("allOf"),
                                    QString::number(index)),
                    issue, depth + 1))
                return false;
        }
    }

    const auto anyOf = schema.value(QStringLiteral("anyOf"));
    if (anyOf.isArray() && !anyOf.toArray().isEmpty())
    {
        agent::ToolValidationIssue firstBranchIssue;
        auto matched = false;
        const auto branches = anyOf.toArray();
        for (qsizetype index = 0; index < branches.size(); ++index)
        {
            const auto branch = branches.at(index);
            if (!branch.isObject()) continue;
            agent::ToolValidationIssue branchIssue;
            if (validateValue(
                    value, branch.toObject(), root, path,
                    childSchemaPath(schemaPath, QStringLiteral("anyOf"),
                                    QString::number(index)),
                    branchIssue, depth + 1))
            {
                matched = true;
                break;
            }
            if (firstBranchIssue.message.isEmpty())
                firstBranchIssue = std::move(branchIssue);
        }
        if (!matched)
        {
            assignIssue(
                issue, path,
                childSchemaPath(schemaPath, QStringLiteral("anyOf")),
                QStringLiteral("anyOf"),
                QStringLiteral("%1 does not match any allowed schema. %2")
                    .arg(path, firstBranchIssue.message));
            return false;
        }
    }

    const auto oneOf = schema.value(QStringLiteral("oneOf"));
    if (oneOf.isArray() && !oneOf.toArray().isEmpty())
    {
        agent::ToolValidationIssue firstBranchIssue;
        agent::ToolValidationIssue discriminatorBranchIssue;
        auto matches = 0;
        const auto branches = oneOf.toArray();
        for (qsizetype index = 0; index < branches.size(); ++index)
        {
            const auto branch = branches.at(index);
            if (!branch.isObject()) continue;
            agent::ToolValidationIssue branchIssue;
            if (validateValue(
                    value, branch.toObject(), root, path,
                    childSchemaPath(schemaPath, QStringLiteral("oneOf"),
                                    QString::number(index)),
                    branchIssue, depth + 1))
                ++matches;
            else
            {
                if (discriminatorBranchIssue.message.isEmpty() &&
                    matchesExplicitDiscriminator(value, schema,
                                                 branch.toObject(), root))
                    discriminatorBranchIssue = branchIssue;
                if (firstBranchIssue.message.isEmpty())
                    firstBranchIssue = std::move(branchIssue);
            }
        }
        if (matches == 0)
        {
            if (!assignDiscriminatorIssue(value, schema, oneOf.toArray(), root,
                                          path, schemaPath, issue))
            {
                if (!discriminatorBranchIssue.message.isEmpty())
                    issue = std::move(discriminatorBranchIssue);
                else if (!firstBranchIssue.message.isEmpty())
                    issue = std::move(firstBranchIssue);
                else
                    assignIssue(
                        issue, path,
                        childSchemaPath(schemaPath, QStringLiteral("oneOf")),
                        QStringLiteral("oneOf"),
                        QStringLiteral("%1 does not match any oneOf branch.")
                            .arg(path));
            }
            return false;
        }
        if (matches > 1)
        {
            assignIssue(issue, path,
                        childSchemaPath(schemaPath, QStringLiteral("oneOf")),
                        QStringLiteral("oneOf"),
                        QStringLiteral("%1 matches more than one oneOf branch.")
                            .arg(path));
            return false;
        }
    }

    const auto typeValue = schema.value(QStringLiteral("type"));
    if ((typeValue.isString() || typeValue.isArray()) &&
        !matchesType(value, typeValue))
    {
        assignIssue(issue, path,
                    childSchemaPath(schemaPath, QStringLiteral("type")),
                    QStringLiteral("type"),
                    QStringLiteral("%1 must be %2.")
                        .arg(path, typeDescription(typeValue)));
        return false;
    }

    if (schema.contains(QStringLiteral("const")) &&
        schema.value(QStringLiteral("const")) != value)
    {
        assignIssue(
            issue, path, childSchemaPath(schemaPath, QStringLiteral("const")),
            QStringLiteral("const"),
            QStringLiteral("%1 must equal %2.")
                .arg(path, compactJson(schema.value(QStringLiteral("const")))));
        return false;
    }

    const auto enumValue = schema.value(QStringLiteral("enum"));
    if (enumValue.isArray() && !enumValue.toArray().contains(value))
    {
        assignIssue(issue, path,
                    childSchemaPath(schemaPath, QStringLiteral("enum")),
                    QStringLiteral("enum"),
                    QStringLiteral("%1 must be one of %2.")
                        .arg(path, compactJson(enumValue.toArray())));
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
                    const auto missingPath =
                        path + QLatin1Char('.') + item.toString();
                    assignIssue(
                        issue, missingPath,
                        childSchemaPath(schemaPath, QStringLiteral("required")),
                        QStringLiteral("required"),
                        QStringLiteral("%1 is required.").arg(missingPath));
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
                    const auto extraPath = path + QLatin1Char('.') + item.key();
                    assignIssue(
                        issue, extraPath,
                        childSchemaPath(schemaPath,
                                        QStringLiteral("additionalProperties")),
                        QStringLiteral("additionalProperties"),
                        QStringLiteral("%1 is not allowed.").arg(extraPath));
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
                    object.value(property.key()), property->toObject(), root,
                    path + QLatin1Char('.') + property.key(),
                    childSchemaPath(schemaPath, QStringLiteral("properties"),
                                    property.key()),
                    issue, depth + 1))
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
            assignIssue(issue, path,
                        childSchemaPath(schemaPath, QStringLiteral("minItems")),
                        QStringLiteral("minItems"),
                        QStringLiteral("%1 has too few items.").arg(path));
            return false;
        }
        if (maximum.isDouble() && array.size() > maximum.toInteger())
        {
            assignIssue(issue, path,
                        childSchemaPath(schemaPath, QStringLiteral("maxItems")),
                        QStringLiteral("maxItems"),
                        QStringLiteral("%1 has too many items.").arg(path));
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
                        childSchemaPath(schemaPath, QStringLiteral("items")),
                        issue, depth + 1))
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
            assignIssue(
                issue, path,
                childSchemaPath(schemaPath, QStringLiteral("minLength")),
                QStringLiteral("minLength"),
                QStringLiteral("%1 is too short.").arg(path));
            return false;
        }
        if (maximum.isDouble() && length > maximum.toInteger())
        {
            assignIssue(
                issue, path,
                childSchemaPath(schemaPath, QStringLiteral("maxLength")),
                QStringLiteral("maxLength"),
                QStringLiteral("%1 is too long.").arg(path));
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
            assignIssue(issue, path,
                        childSchemaPath(schemaPath, QStringLiteral("minimum")),
                        QStringLiteral("minimum"),
                        QStringLiteral("%1 is below the minimum.").arg(path));
            return false;
        }
        if (maximum.isDouble() && number > maximum.toDouble())
        {
            assignIssue(issue, path,
                        childSchemaPath(schemaPath, QStringLiteral("maximum")),
                        QStringLiteral("maximum"),
                        QStringLiteral("%1 exceeds the maximum.").arg(path));
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
    const auto result = validateArgumentsDetailed(qualifiedName, arguments);
    errorMessage = result.valid ? QString{} : result.issue.message;
    return result.valid;
}

agent::ToolValidationResult ToolRegistry::validateArgumentsDetailed(
    const QString& qualifiedName, const QJsonObject& arguments) const
{
    const auto* definition = find(qualifiedName);
    if (definition == nullptr)
    {
        return {
            false,
            {qualifiedName,
             QStringLiteral("arguments"),
             {},
             QStringLiteral("tool"),
             QStringLiteral("Tool is not registered: %1").arg(qualifiedName)}};
    }
    agent::ToolValidationIssue issue;
    const auto valid = validateValue(
        arguments, definition->inputSchema, definition->inputSchema,
        QStringLiteral("arguments"), QStringLiteral("#"), issue);
    issue.toolName = qualifiedName;
    return {valid, std::move(issue)};
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
    agent::ToolValidationIssue issue;
    const auto valid = validateValue(
        output, definition->outputSchema, definition->outputSchema,
        QStringLiteral("structuredContent"), QStringLiteral("#"), issue);
    errorMessage = valid ? QString{} : issue.message;
    return valid;
}
}  // namespace qtllm::infrastructure::mcp
