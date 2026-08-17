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
    connect(&runtime_, &McpHostRuntime::toolResultReady, this,
            &McpClientManager::toolResultReady);
    connect(&runtime_, &McpHostRuntime::requestFailed, this,
            &McpClientManager::requestFailed);
    connect(&runtime_, &McpHostRuntime::notificationReceived, this,
            &McpClientManager::notificationReceived);
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

QString McpClientManager::listTools(const QString& serverId)
{
    return runtime_.listTools(serverId);
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
