#include "McpCatalog.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <functional>

namespace qtllm::infrastructure::mcp
{
namespace
{
constexpr qsizetype maximumCatalogEntries = 2'048;
constexpr qsizetype maximumUriCharacters = 4'096;
constexpr qsizetype maximumNameCharacters = 256;
constexpr qsizetype maximumDescriptionCharacters = 4'096;
constexpr qsizetype maximumMimeTypeCharacters = 256;
constexpr qsizetype maximumResourceContents = 64;
constexpr qsizetype maximumPromptArguments = 64;
constexpr qsizetype maximumPromptMessages = 128;

bool validOptionalString(const QJsonValue& value, qsizetype maximum)
{
    return value.isUndefined() ||
           (value.isString() && value.toString().size() <= maximum);
}

bool validUri(const QString& uri)
{
    return !uri.isEmpty() && uri.size() <= maximumUriCharacters &&
           QUrl(uri, QUrl::StrictMode).isValid() && !QUrl(uri).isRelative();
}

template <typename Definition>
void sortDefinitions(QList<Definition>& definitions,
                     const std::function<QString(const Definition&)>& key)
{
    std::sort(definitions.begin(), definitions.end(),
              [&key](const Definition& left, const Definition& right)
              {
                  const auto serverOrder = QString::compare(
                      left.serverId, right.serverId, Qt::CaseSensitive);
                  return serverOrder == 0
                             ? QString::compare(key(left), key(right),
                                                Qt::CaseSensitive) < 0
                             : serverOrder < 0;
              });
}

bool parseResourceDefinition(const QString& serverId, const QJsonValue& value,
                             McpResourceDefinition& definition,
                             QString& errorMessage)
{
    if (!value.isObject())
    {
        errorMessage =
            QStringLiteral("resources/list contains a non-object resource.");
        return false;
    }
    const auto object = value.toObject();
    const auto uriValue = object.value(QStringLiteral("uri"));
    const auto nameValue = object.value(QStringLiteral("name"));
    const auto description = object.value(QStringLiteral("description"));
    const auto mimeType = object.value(QStringLiteral("mimeType"));
    const auto annotations = object.value(QStringLiteral("annotations"));
    const auto uri = uriValue.toString().trimmed();
    const auto name = nameValue.toString().trimmed();
    if (!uriValue.isString() || !validUri(uri) || !nameValue.isString() ||
        name.isEmpty() || name.size() > maximumNameCharacters ||
        !validOptionalString(description, maximumDescriptionCharacters) ||
        !validOptionalString(mimeType, maximumMimeTypeCharacters) ||
        (!annotations.isUndefined() && !annotations.isObject()))
    {
        errorMessage = QStringLiteral(
            "resources/list contains an invalid resource definition.");
        return false;
    }
    definition = {serverId,
                  uri,
                  name,
                  description.toString(),
                  mimeType.toString(),
                  annotations.toObject()};
    return true;
}

bool parseTemplateDefinition(const QString& serverId, const QJsonValue& value,
                             McpResourceTemplateDefinition& definition,
                             QString& errorMessage)
{
    if (!value.isObject())
    {
        errorMessage = QStringLiteral(
            "resources/templates/list contains a non-object template.");
        return false;
    }
    const auto object = value.toObject();
    const auto uriTemplate = object.value(QStringLiteral("uriTemplate"));
    const auto nameValue = object.value(QStringLiteral("name"));
    const auto description = object.value(QStringLiteral("description"));
    const auto mimeType = object.value(QStringLiteral("mimeType"));
    const auto annotations = object.value(QStringLiteral("annotations"));
    const auto templateText = uriTemplate.toString().trimmed();
    const auto name = nameValue.toString().trimmed();
    if (!uriTemplate.isString() || templateText.isEmpty() ||
        templateText.size() > maximumUriCharacters || !nameValue.isString() ||
        name.isEmpty() || name.size() > maximumNameCharacters ||
        !validOptionalString(description, maximumDescriptionCharacters) ||
        !validOptionalString(mimeType, maximumMimeTypeCharacters) ||
        (!annotations.isUndefined() && !annotations.isObject()))
    {
        errorMessage = QStringLiteral(
            "resources/templates/list contains an invalid template.");
        return false;
    }
    definition = {serverId,
                  templateText,
                  name,
                  description.toString(),
                  mimeType.toString(),
                  annotations.toObject()};
    return true;
}

bool parsePromptDefinition(const QString& serverId, const QJsonValue& value,
                           McpPromptDefinition& definition,
                           QString& errorMessage)
{
    if (!value.isObject())
    {
        errorMessage =
            QStringLiteral("prompts/list contains a non-object prompt.");
        return false;
    }
    const auto object = value.toObject();
    const auto nameValue = object.value(QStringLiteral("name"));
    const auto description = object.value(QStringLiteral("description"));
    const auto arguments = object.value(QStringLiteral("arguments"));
    const auto name = nameValue.toString().trimmed();
    if (!nameValue.isString() || name.isEmpty() ||
        name.size() > maximumNameCharacters ||
        !validOptionalString(description, maximumDescriptionCharacters) ||
        (!arguments.isUndefined() && !arguments.isArray()) ||
        arguments.toArray().size() > maximumPromptArguments)
    {
        errorMessage =
            QStringLiteral("prompts/list contains an invalid prompt.");
        return false;
    }

    QList<McpPromptArgument> parsedArguments;
    QSet<QString> argumentNames;
    for (const auto& argumentValue : arguments.toArray())
    {
        if (!argumentValue.isObject())
        {
            errorMessage = QStringLiteral(
                               "Prompt %1.%2 has an invalid "
                               "argument definition.")
                               .arg(serverId, name);
            return false;
        }
        const auto argument = argumentValue.toObject();
        const auto argumentNameValue = argument.value(QStringLiteral("name"));
        const auto argumentDescription =
            argument.value(QStringLiteral("description"));
        const auto required = argument.value(QStringLiteral("required"));
        const auto argumentName = argumentNameValue.toString().trimmed();
        if (!argumentNameValue.isString() || argumentName.isEmpty() ||
            argumentName.size() > maximumNameCharacters ||
            argumentNames.contains(argumentName) ||
            !validOptionalString(argumentDescription,
                                 maximumDescriptionCharacters) ||
            (!required.isUndefined() && !required.isBool()))
        {
            errorMessage = QStringLiteral(
                               "Prompt %1.%2 has an invalid "
                               "argument definition.")
                               .arg(serverId, name);
            return false;
        }
        argumentNames.insert(argumentName);
        parsedArguments.append(
            {argumentName, argumentDescription.toString(), required.toBool()});
    }
    definition = {serverId, name, description.toString(), parsedArguments};
    return true;
}

bool validatePromptContent(const QJsonObject& content)
{
    const auto type = content.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("text"))
        return content.value(QStringLiteral("text")).isString();
    if (type == QLatin1String("image"))
        return content.value(QStringLiteral("data")).isString() &&
               content.value(QStringLiteral("mimeType")).isString();
    if (type == QLatin1String("resource"))
        return content.value(QStringLiteral("resource")).isObject();
    return false;
}
}  // namespace

bool parseResourcePage(const QString& serverId, const QJsonObject& result,
                       const QList<McpResourceDefinition>& accumulated,
                       QList<McpResourceDefinition>& definitions,
                       QString& errorMessage)
{
    const auto values = result.value(QStringLiteral("resources"));
    if (!values.isArray())
    {
        errorMessage =
            QStringLiteral("resources/list result has no resources array.");
        return false;
    }
    if (accumulated.size() + values.toArray().size() > maximumCatalogEntries)
    {
        errorMessage = QStringLiteral("MCP resource catalog is too large.");
        return false;
    }

    definitions = accumulated;
    QSet<QString> uris;
    for (const auto& definition : accumulated)
        uris.insert(definition.uri);
    for (const auto& value : values.toArray())
    {
        McpResourceDefinition definition;
        if (!parseResourceDefinition(serverId, value, definition, errorMessage))
            return false;
        if (uris.contains(definition.uri))
        {
            errorMessage = QStringLiteral("Duplicate resource URI: %1")
                               .arg(definition.uri);
            return false;
        }
        uris.insert(definition.uri);
        definitions.append(std::move(definition));
    }
    return true;
}

bool parseResourceTemplatePage(
    const QString& serverId, const QJsonObject& result,
    const QList<McpResourceTemplateDefinition>& accumulated,
    QList<McpResourceTemplateDefinition>& definitions, QString& errorMessage)
{
    const auto values = result.value(QStringLiteral("resourceTemplates"));
    if (!values.isArray())
    {
        errorMessage = QStringLiteral(
            "resources/templates/list result has no resourceTemplates array.");
        return false;
    }
    if (accumulated.size() + values.toArray().size() > maximumCatalogEntries)
    {
        errorMessage =
            QStringLiteral("MCP resource template catalog is too large.");
        return false;
    }

    definitions = accumulated;
    QSet<QString> templates;
    for (const auto& definition : accumulated)
        templates.insert(definition.uriTemplate);
    for (const auto& value : values.toArray())
    {
        McpResourceTemplateDefinition definition;
        if (!parseTemplateDefinition(serverId, value, definition, errorMessage))
            return false;
        if (templates.contains(definition.uriTemplate))
        {
            errorMessage = QStringLiteral("Duplicate resource template: %1")
                               .arg(definition.uriTemplate);
            return false;
        }
        templates.insert(definition.uriTemplate);
        definitions.append(std::move(definition));
    }
    return true;
}

bool parsePromptPage(const QString& serverId, const QJsonObject& result,
                     const QList<McpPromptDefinition>& accumulated,
                     QList<McpPromptDefinition>& definitions,
                     QString& errorMessage)
{
    const auto values = result.value(QStringLiteral("prompts"));
    if (!values.isArray())
    {
        errorMessage =
            QStringLiteral("prompts/list result has no prompts array.");
        return false;
    }
    if (accumulated.size() + values.toArray().size() > maximumCatalogEntries)
    {
        errorMessage = QStringLiteral("MCP prompt catalog is too large.");
        return false;
    }

    definitions = accumulated;
    QSet<QString> names;
    for (const auto& definition : accumulated)
        names.insert(definition.name);
    for (const auto& value : values.toArray())
    {
        McpPromptDefinition definition;
        if (!parsePromptDefinition(serverId, value, definition, errorMessage))
            return false;
        if (names.contains(definition.name))
        {
            errorMessage = QStringLiteral("Duplicate prompt: %1.%2")
                               .arg(serverId, definition.name);
            return false;
        }
        names.insert(definition.name);
        definitions.append(std::move(definition));
    }
    return true;
}

bool parseResourceReadResult(const QString& requestId, const QString& serverId,
                             const QString& requestedUri,
                             const QJsonObject& result, qsizetype maximumBytes,
                             McpResourceReadResult& parsed,
                             QString& errorMessage)
{
    const auto serialized =
        QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (serialized.size() > maximumBytes)
    {
        errorMessage = QStringLiteral("MCP resource result exceeds %1 bytes.")
                           .arg(maximumBytes);
        return false;
    }
    const auto values = result.value(QStringLiteral("contents"));
    if (!values.isArray() || values.toArray().size() > maximumResourceContents)
    {
        errorMessage = QStringLiteral(
            "resources/read result has an invalid contents array.");
        return false;
    }

    McpResourceReadResult output;
    output.requestId = requestId;
    output.serverId = serverId;
    output.requestedUri = requestedUri;
    output.originalBytes = serialized.size();
    for (const auto& value : values.toArray())
    {
        if (!value.isObject())
        {
            errorMessage = QStringLiteral(
                "resources/read contains a non-object content item.");
            return false;
        }
        const auto object = value.toObject();
        const auto uriValue = object.value(QStringLiteral("uri"));
        const auto mimeType = object.value(QStringLiteral("mimeType"));
        const auto text = object.value(QStringLiteral("text"));
        const auto blob = object.value(QStringLiteral("blob"));
        const auto uri = uriValue.toString().trimmed();
        if (!uriValue.isString() || !validUri(uri) ||
            !validOptionalString(mimeType, maximumMimeTypeCharacters) ||
            (text.isString() == blob.isString()))
        {
            errorMessage = QStringLiteral(
                "resources/read contains an invalid content item.");
            return false;
        }
        McpResourceContent content;
        content.uri = uri;
        content.mimeType = mimeType.toString();
        content.binary = blob.isString();
        if (content.binary)
            content.blob = blob.toString().toLatin1();
        else
            content.text = text.toString();
        output.contents.append(std::move(content));
    }
    parsed = std::move(output);
    return true;
}

bool parsePromptResult(const QString& requestId, const QString& serverId,
                       const QString& promptName, const QJsonObject& result,
                       qsizetype maximumBytes, McpPromptResult& parsed,
                       QString& errorMessage)
{
    const auto serialized =
        QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (serialized.size() > maximumBytes)
    {
        errorMessage = QStringLiteral("MCP prompt result exceeds %1 bytes.")
                           .arg(maximumBytes);
        return false;
    }
    const auto description = result.value(QStringLiteral("description"));
    const auto messages = result.value(QStringLiteral("messages"));
    if (!validOptionalString(description, maximumDescriptionCharacters) ||
        !messages.isArray() ||
        messages.toArray().size() > maximumPromptMessages)
    {
        errorMessage =
            QStringLiteral("prompts/get result has invalid messages.");
        return false;
    }

    McpPromptResult output;
    output.requestId = requestId;
    output.serverId = serverId;
    output.promptName = promptName;
    output.description = description.toString();
    output.originalBytes = serialized.size();
    for (const auto& value : messages.toArray())
    {
        if (!value.isObject())
        {
            errorMessage =
                QStringLiteral("prompts/get contains a non-object message.");
            return false;
        }
        const auto object = value.toObject();
        const auto role = object.value(QStringLiteral("role")).toString();
        const auto content = object.value(QStringLiteral("content"));
        if ((role != QLatin1String("user") &&
             role != QLatin1String("assistant")) ||
            !content.isObject() || !validatePromptContent(content.toObject()))
        {
            errorMessage =
                QStringLiteral("prompts/get contains an invalid message.");
            return false;
        }
        output.messages.append({role, content.toObject()});
    }
    parsed = std::move(output);
    return true;
}

bool McpResourceRegistry::replaceServerResources(
    const QString& serverId, const QList<McpResourceDefinition>& definitions,
    QString& errorMessage)
{
    if (serverId.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Resource serverId must not be empty.");
        return false;
    }
    QHash<QString, McpResourceDefinition> replacement;
    for (const auto& definition : definitions)
    {
        if (definition.serverId != serverId ||
            replacement.contains(definition.uri))
        {
            errorMessage = QStringLiteral(
                               "Invalid or duplicate resource for "
                               "Server %1.")
                               .arg(serverId);
            return false;
        }
        replacement.insert(definition.uri, definition);
    }
    resources_.insert(serverId, replacement);
    return true;
}

bool McpResourceRegistry::replaceServerTemplates(
    const QString& serverId,
    const QList<McpResourceTemplateDefinition>& definitions,
    QString& errorMessage)
{
    if (serverId.trimmed().isEmpty())
    {
        errorMessage =
            QStringLiteral("Resource template serverId must not be empty.");
        return false;
    }
    QHash<QString, McpResourceTemplateDefinition> replacement;
    for (const auto& definition : definitions)
    {
        if (definition.serverId != serverId ||
            replacement.contains(definition.uriTemplate))
        {
            errorMessage = QStringLiteral(
                               "Invalid or duplicate resource template for "
                               "Server %1.")
                               .arg(serverId);
            return false;
        }
        replacement.insert(definition.uriTemplate, definition);
    }
    templates_.insert(serverId, replacement);
    return true;
}

void McpResourceRegistry::removeServer(const QString& serverId)
{
    resources_.remove(serverId);
    templates_.remove(serverId);
}

QList<McpResourceDefinition> McpResourceRegistry::resources(
    const QString& serverId) const
{
    QList<McpResourceDefinition> result;
    if (!serverId.isEmpty())
        result = resources_.value(serverId).values();
    else
        for (const auto& serverResources : resources_)
            result.append(serverResources.values());
    sortDefinitions<McpResourceDefinition>(
        result, [](const auto& definition) { return definition.uri; });
    return result;
}

QList<McpResourceTemplateDefinition> McpResourceRegistry::templates(
    const QString& serverId) const
{
    QList<McpResourceTemplateDefinition> result;
    if (!serverId.isEmpty())
        result = templates_.value(serverId).values();
    else
        for (const auto& serverTemplates : templates_)
            result.append(serverTemplates.values());
    sortDefinitions<McpResourceTemplateDefinition>(
        result, [](const auto& definition) { return definition.uriTemplate; });
    return result;
}

const McpResourceDefinition* McpResourceRegistry::find(const QString& serverId,
                                                       const QString& uri) const
{
    const auto server = resources_.constFind(serverId);
    if (server == resources_.constEnd()) return nullptr;
    const auto resource = server->constFind(uri);
    return resource == server->constEnd() ? nullptr : &resource.value();
}

bool McpPromptRegistry::replaceServerPrompts(
    const QString& serverId, const QList<McpPromptDefinition>& definitions,
    QString& errorMessage)
{
    if (serverId.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Prompt serverId must not be empty.");
        return false;
    }
    QHash<QString, McpPromptDefinition> replacement;
    for (const auto& definition : definitions)
    {
        if (definition.serverId != serverId ||
            replacement.contains(definition.name))
        {
            errorMessage = QStringLiteral(
                               "Invalid or duplicate prompt for "
                               "Server %1.")
                               .arg(serverId);
            return false;
        }
        replacement.insert(definition.name, definition);
    }
    prompts_.insert(serverId, replacement);
    return true;
}

void McpPromptRegistry::removeServer(const QString& serverId)
{
    prompts_.remove(serverId);
}

QList<McpPromptDefinition> McpPromptRegistry::prompts(
    const QString& serverId) const
{
    QList<McpPromptDefinition> result;
    if (!serverId.isEmpty())
        result = prompts_.value(serverId).values();
    else
        for (const auto& serverPrompts : prompts_)
            result.append(serverPrompts.values());
    sortDefinitions<McpPromptDefinition>(
        result, [](const auto& definition) { return definition.name; });
    return result;
}

const McpPromptDefinition* McpPromptRegistry::find(const QString& serverId,
                                                   const QString& name) const
{
    const auto server = prompts_.constFind(serverId);
    if (server == prompts_.constEnd()) return nullptr;
    const auto prompt = server->constFind(name);
    return prompt == server->constEnd() ? nullptr : &prompt.value();
}

bool McpPromptRegistry::validateArguments(const QString& serverId,
                                          const QString& name,
                                          const QJsonObject& arguments,
                                          QString& errorMessage) const
{
    const auto* prompt = find(serverId, name);
    if (prompt == nullptr)
    {
        errorMessage = QStringLiteral("Prompt is not registered: %1.%2")
                           .arg(serverId, name);
        return false;
    }
    QSet<QString> known;
    for (const auto& argument : prompt->arguments)
    {
        known.insert(argument.name);
        if (argument.required && !arguments.contains(argument.name))
        {
            errorMessage = QStringLiteral("Prompt argument '%1' is required.")
                               .arg(argument.name);
            return false;
        }
    }
    for (auto argument = arguments.constBegin();
         argument != arguments.constEnd(); ++argument)
    {
        if (!known.contains(argument.key()))
        {
            errorMessage = QStringLiteral("Prompt argument '%1' is unknown.")
                               .arg(argument.key());
            return false;
        }
        if (!argument->isString())
        {
            errorMessage = QStringLiteral(
                               "Prompt argument '%1' must be a "
                               "string.")
                               .arg(argument.key());
            return false;
        }
    }
    return true;
}

bool McpRootRegistry::replaceServerRoots(const QString& serverId,
                                         const QStringList& authorizedPaths,
                                         QString& errorMessage)
{
    if (serverId.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Root serverId must not be empty.");
        return false;
    }
    QList<McpRoot> replacement;
    QSet<QString> uris;
    for (const auto& path : authorizedPaths)
    {
        const QFileInfo info(path);
        const auto canonical = info.canonicalFilePath();
        if (!info.isAbsolute() || !info.isDir() || canonical.isEmpty())
        {
            errorMessage = QStringLiteral(
                               "Authorized MCP root is not an existing "
                               "absolute directory: %1")
                               .arg(path);
            return false;
        }
        const auto uri =
            QUrl::fromLocalFile(canonical).toString(QUrl::FullyEncoded);
        if (uris.contains(uri)) continue;
        uris.insert(uri);
        auto name = QFileInfo(canonical).fileName();
        if (name.isEmpty()) name = canonical;
        replacement.append({uri, name, canonical});
    }
    std::sort(replacement.begin(), replacement.end(),
              [](const McpRoot& left, const McpRoot& right)
              { return left.uri < right.uri; });
    roots_.insert(serverId, replacement);
    return true;
}

void McpRootRegistry::removeServer(const QString& serverId)
{
    roots_.remove(serverId);
}

QList<McpRoot> McpRootRegistry::roots(const QString& serverId) const
{
    return roots_.value(serverId);
}
}  // namespace qtllm::infrastructure::mcp
