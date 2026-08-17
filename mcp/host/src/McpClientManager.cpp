#include "McpClientManager.hpp"

namespace qtllm::infrastructure::mcp
{
McpClientManager::McpClientManager(QObject* parent) : QObject(parent)
{
    connect(&runtime_, &McpHostRuntime::serverStateChanged, this,
            &McpClientManager::serverStateChanged);
    connect(&runtime_, &McpHostRuntime::capabilitySnapshotChanged, this,
            &McpClientManager::capabilitySnapshotChanged);
    connect(&runtime_, &McpHostRuntime::serverStarted, this,
            &McpClientManager::serverStarted);
    connect(&runtime_, &McpHostRuntime::serverStopped, this,
            &McpClientManager::serverStopped);
    connect(&runtime_, &McpHostRuntime::serverInitialized, this,
            &McpClientManager::serverInitialized);
    connect(&runtime_, &McpHostRuntime::toolsChanged, this,
            &McpClientManager::toolsChanged);
    connect(&runtime_, &McpHostRuntime::resourcesChanged, this,
            &McpClientManager::resourcesChanged);
    connect(&runtime_, &McpHostRuntime::resourceTemplatesChanged, this,
            &McpClientManager::resourceTemplatesChanged);
    connect(&runtime_, &McpHostRuntime::resourceReadReady, this,
            &McpClientManager::resourceReadReady);
    connect(&runtime_, &McpHostRuntime::resourceSubscriptionChanged, this,
            &McpClientManager::resourceSubscriptionChanged);
    connect(&runtime_, &McpHostRuntime::resourceUpdated, this,
            &McpClientManager::resourceUpdated);
    connect(&runtime_, &McpHostRuntime::promptsChanged, this,
            &McpClientManager::promptsChanged);
    connect(&runtime_, &McpHostRuntime::promptReady, this,
            &McpClientManager::promptReady);
    connect(&runtime_, &McpHostRuntime::completionReady, this,
            &McpClientManager::completionReady);
    connect(&runtime_, &McpHostRuntime::loggingLevelChanged, this,
            &McpClientManager::loggingLevelChanged);
    connect(&runtime_, &McpHostRuntime::rootsRequested, this,
            &McpClientManager::rootsRequested);
    connect(&runtime_, &McpHostRuntime::pingCompleted, this,
            &McpClientManager::pingCompleted);
    connect(&runtime_, &McpHostRuntime::pingRequested, this,
            &McpClientManager::pingRequested);
    connect(&runtime_, &McpHostRuntime::toolResultReady, this,
            &McpClientManager::toolResultReady);
    connect(&runtime_, &McpHostRuntime::requestFailed, this,
            &McpClientManager::requestFailed);
    connect(&runtime_, &McpHostRuntime::notificationReceived, this,
            &McpClientManager::notificationReceived);
    connect(&runtime_, &McpHostRuntime::progressReceived, this,
            &McpClientManager::progressReceived);
    connect(&runtime_, &McpHostRuntime::loggingMessageReceived, this,
            &McpClientManager::loggingMessageReceived);
    connect(&runtime_, &McpHostRuntime::diagnosticReceived, this,
            &McpClientManager::diagnosticReceived);
    connect(&runtime_, &McpHostRuntime::serverError, this,
            &McpClientManager::serverError);
}

McpClientManager::~McpClientManager() = default;

bool McpClientManager::addServer(McpServerConfig config, QString& errorMessage)
{
    return runtime_.addServer(std::move(config), errorMessage);
}

bool McpClientManager::removeServer(const QString& serverId,
                                    QString& errorMessage)
{
    return runtime_.removeServer(serverId, errorMessage);
}

QStringList McpClientManager::serverIds() const
{
    return runtime_.serverIds();
}

StdioMcpTransport* McpClientManager::transport(const QString& serverId) const
{
    return qobject_cast<StdioMcpTransport*>(runtime_.transport(serverId));
}

QList<agent::ToolDefinition> McpClientManager::tools() const
{
    return runtime_.tools();
}

QList<McpResourceDefinition> McpClientManager::resources(
    const QString& serverId) const
{
    return runtime_.resources(serverId);
}

QList<McpResourceTemplateDefinition> McpClientManager::resourceTemplates(
    const QString& serverId) const
{
    return runtime_.resourceTemplates(serverId);
}

QList<McpPromptDefinition> McpClientManager::prompts(
    const QString& serverId) const
{
    return runtime_.prompts(serverId);
}

QList<McpRoot> McpClientManager::roots(const QString& serverId) const
{
    return runtime_.roots(serverId);
}

bool McpClientManager::isResourceSubscribed(const QString& serverId,
                                            const QString& uri) const
{
    return runtime_.isResourceSubscribed(serverId, uri);
}

QString McpClientManager::agentInstructions() const
{
    return runtime_.agentInstructions();
}

const ToolRegistry& McpClientManager::registry() const
{
    return runtime_.registry();
}

std::optional<McpServerSnapshot> McpClientManager::serverSnapshot(
    const QString& serverId) const
{
    return runtime_.serverSnapshot(serverId);
}

QList<McpServerSnapshot> McpClientManager::serverSnapshots() const
{
    return runtime_.serverSnapshots();
}

bool McpClientManager::setUseInstructions(const QString& serverId, bool enabled,
                                          QString& errorMessage)
{
    return runtime_.setUseInstructions(serverId, enabled, errorMessage);
}

void McpClientManager::startServer(const QString& serverId)
{
    runtime_.startServer(serverId);
}

void McpClientManager::stopServer(const QString& serverId)
{
    runtime_.stopServer(serverId);
}

QString McpClientManager::initialize(const QString& serverId)
{
    return runtime_.initialize(serverId);
}

QString McpClientManager::ping(const QString& serverId)
{
    return runtime_.ping(serverId);
}

QString McpClientManager::listTools(const QString& serverId)
{
    return runtime_.listTools(serverId);
}

QString McpClientManager::listResources(const QString& serverId)
{
    return runtime_.listResources(serverId);
}

QString McpClientManager::listResourceTemplates(const QString& serverId)
{
    return runtime_.listResourceTemplates(serverId);
}

QString McpClientManager::readResource(const QString& serverId,
                                       const QString& uri)
{
    return runtime_.readResource(serverId, uri);
}

QString McpClientManager::subscribeResource(const QString& serverId,
                                            const QString& uri)
{
    return runtime_.subscribeResource(serverId, uri);
}

QString McpClientManager::unsubscribeResource(const QString& serverId,
                                              const QString& uri)
{
    return runtime_.unsubscribeResource(serverId, uri);
}

QString McpClientManager::listPrompts(const QString& serverId)
{
    return runtime_.listPrompts(serverId);
}

QString McpClientManager::getPrompt(const QString& serverId,
                                    const QString& name,
                                    const QJsonObject& arguments)
{
    return runtime_.getPrompt(serverId, name, arguments);
}

QString McpClientManager::completePrompt(const QString& serverId,
                                         const QString& promptName,
                                         const QString& argumentName,
                                         const QString& value,
                                         const QJsonObject& contextArguments)
{
    return runtime_.completePrompt(serverId, promptName, argumentName, value,
                                   contextArguments);
}

QString McpClientManager::completeResourceTemplate(
    const QString& serverId, const QString& uriTemplate,
    const QString& argumentName, const QString& value,
    const QJsonObject& contextArguments)
{
    return runtime_.completeResourceTemplate(
        serverId, uriTemplate, argumentName, value, contextArguments);
}

QString McpClientManager::setLoggingLevel(const QString& serverId,
                                          const QString& level)
{
    return runtime_.setLoggingLevel(serverId, level);
}

QString McpClientManager::callTool(const QString& qualifiedToolName,
                                   const QJsonObject& arguments)
{
    return runtime_.callTool(qualifiedToolName, arguments);
}

void McpClientManager::cancel(const QString& requestId)
{
    runtime_.cancel(requestId);
}

void McpClientManager::cancelAll()
{
    runtime_.cancelAll();
}
}  // namespace qtllm::infrastructure::mcp
