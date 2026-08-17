#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace qtllm::infrastructure::mcp
{
struct McpResourceDefinition
{
    QString serverId;
    QString uri;
    QString name;
    QString description;
    QString mimeType;
    QJsonObject annotations;
};

struct McpResourceTemplateDefinition
{
    QString serverId;
    QString uriTemplate;
    QString name;
    QString description;
    QString mimeType;
    QJsonObject annotations;
};

struct McpResourceContent
{
    QString uri;
    QString mimeType;
    QString text;
    QByteArray blob;
    bool binary = false;
};

struct McpResourceReadResult
{
    QString requestId;
    QString serverId;
    QString requestedUri;
    QList<McpResourceContent> contents;
    qsizetype originalBytes = 0;
};

struct McpPromptArgument
{
    QString name;
    QString description;
    bool required = false;
};

struct McpPromptDefinition
{
    QString serverId;
    QString name;
    QString description;
    QList<McpPromptArgument> arguments;
};

struct McpPromptMessage
{
    QString role;
    QJsonObject content;
};

struct McpPromptResult
{
    QString requestId;
    QString serverId;
    QString promptName;
    QString description;
    QList<McpPromptMessage> messages;
    qsizetype originalBytes = 0;
};

struct McpRoot
{
    QString uri;
    QString name;
    QString canonicalPath;
};

bool parseResourcePage(const QString& serverId, const QJsonObject& result,
                       const QList<McpResourceDefinition>& accumulated,
                       QList<McpResourceDefinition>& definitions,
                       QString& errorMessage);
bool parseResourceTemplatePage(
    const QString& serverId, const QJsonObject& result,
    const QList<McpResourceTemplateDefinition>& accumulated,
    QList<McpResourceTemplateDefinition>& definitions, QString& errorMessage);
bool parsePromptPage(const QString& serverId, const QJsonObject& result,
                     const QList<McpPromptDefinition>& accumulated,
                     QList<McpPromptDefinition>& definitions,
                     QString& errorMessage);
bool parseResourceReadResult(const QString& requestId, const QString& serverId,
                             const QString& requestedUri,
                             const QJsonObject& result, qsizetype maximumBytes,
                             McpResourceReadResult& parsed,
                             QString& errorMessage);
bool parsePromptResult(const QString& requestId, const QString& serverId,
                       const QString& promptName, const QJsonObject& result,
                       qsizetype maximumBytes, McpPromptResult& parsed,
                       QString& errorMessage);

class McpResourceRegistry final
{
   public:
    bool replaceServerResources(const QString& serverId,
                                const QList<McpResourceDefinition>& definitions,
                                QString& errorMessage);
    bool replaceServerTemplates(
        const QString& serverId,
        const QList<McpResourceTemplateDefinition>& definitions,
        QString& errorMessage);
    void removeServer(const QString& serverId);

    [[nodiscard]] QList<McpResourceDefinition> resources(
        const QString& serverId = {}) const;
    [[nodiscard]] QList<McpResourceTemplateDefinition> templates(
        const QString& serverId = {}) const;
    [[nodiscard]] const McpResourceDefinition* find(const QString& serverId,
                                                    const QString& uri) const;

   private:
    QHash<QString, QHash<QString, McpResourceDefinition>> resources_;
    QHash<QString, QHash<QString, McpResourceTemplateDefinition>> templates_;
};

class McpPromptRegistry final
{
   public:
    bool replaceServerPrompts(const QString& serverId,
                              const QList<McpPromptDefinition>& definitions,
                              QString& errorMessage);
    void removeServer(const QString& serverId);

    [[nodiscard]] QList<McpPromptDefinition> prompts(
        const QString& serverId = {}) const;
    [[nodiscard]] const McpPromptDefinition* find(const QString& serverId,
                                                  const QString& name) const;
    [[nodiscard]] bool validateArguments(const QString& serverId,
                                         const QString& name,
                                         const QJsonObject& arguments,
                                         QString& errorMessage) const;

   private:
    QHash<QString, QHash<QString, McpPromptDefinition>> prompts_;
};

class McpRootRegistry final
{
   public:
    bool replaceServerRoots(const QString& serverId,
                            const QStringList& authorizedPaths,
                            QString& errorMessage);
    void removeServer(const QString& serverId);
    [[nodiscard]] QList<McpRoot> roots(const QString& serverId) const;

   private:
    QHash<QString, QList<McpRoot>> roots_;
};
}  // namespace qtllm::infrastructure::mcp

Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpResourceDefinition)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpResourceTemplateDefinition)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpResourceReadResult)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpPromptDefinition)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpPromptResult)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpRoot)
