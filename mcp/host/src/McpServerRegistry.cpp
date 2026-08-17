#include "McpServerRegistry.hpp"

#include <algorithm>

namespace qtllm::infrastructure::mcp
{
namespace
{
bool transitionAllowed(McpServerState from, McpServerState to)
{
    if (from == to) return true;
    switch (from)
    {
        case McpServerState::Stopped:
            return to == McpServerState::Starting;
        case McpServerState::Starting:
            return to == McpServerState::Initializing ||
                   to == McpServerState::Stopping ||
                   to == McpServerState::Failed;
        case McpServerState::Initializing:
            return to == McpServerState::Ready ||
                   to == McpServerState::Stopping ||
                   to == McpServerState::Failed;
        case McpServerState::Ready:
            return to == McpServerState::Degraded ||
                   to == McpServerState::Stopping ||
                   to == McpServerState::Failed;
        case McpServerState::Degraded:
            return to == McpServerState::Ready ||
                   to == McpServerState::Stopping ||
                   to == McpServerState::Failed;
        case McpServerState::Failed:
            return to == McpServerState::Starting ||
                   to == McpServerState::Stopping ||
                   to == McpServerState::Stopped;
        case McpServerState::Stopping:
            return to == McpServerState::Stopped ||
                   to == McpServerState::Failed;
    }
    return false;
}
}  // namespace

QString mcpServerStateName(McpServerState state)
{
    switch (state)
    {
        case McpServerState::Stopped:
            return QStringLiteral("stopped");
        case McpServerState::Starting:
            return QStringLiteral("starting");
        case McpServerState::Initializing:
            return QStringLiteral("initializing");
        case McpServerState::Ready:
            return QStringLiteral("ready");
        case McpServerState::Degraded:
            return QStringLiteral("degraded");
        case McpServerState::Failed:
            return QStringLiteral("failed");
        case McpServerState::Stopping:
            return QStringLiteral("stopping");
    }
    return QStringLiteral("unknown");
}

bool McpServerRegistry::addServer(const QString& serverId,
                                  QString& errorMessage)
{
    const auto normalized = serverId.trimmed();
    if (normalized.isEmpty())
    {
        errorMessage = QStringLiteral("MCP serverId must not be empty.");
        return false;
    }
    if (servers_.contains(normalized))
    {
        errorMessage = QStringLiteral("MCP serverId is already registered: %1")
                           .arg(normalized);
        return false;
    }
    McpServerSnapshot snapshot;
    snapshot.serverId = normalized;
    servers_.insert(normalized, snapshot);
    return true;
}

bool McpServerRegistry::removeServer(const QString& serverId)
{
    return servers_.remove(serverId);
}

bool McpServerRegistry::contains(const QString& serverId) const
{
    return servers_.contains(serverId);
}

QStringList McpServerRegistry::serverIds() const
{
    auto result = servers_.keys();
    result.sort(Qt::CaseSensitive);
    return result;
}

std::optional<McpServerSnapshot> McpServerRegistry::snapshot(
    const QString& serverId) const
{
    const auto iterator = servers_.constFind(serverId);
    if (iterator == servers_.constEnd()) return std::nullopt;
    return iterator.value();
}

QList<McpServerSnapshot> McpServerRegistry::snapshots() const
{
    auto result = servers_.values();
    std::sort(result.begin(), result.end(),
              [](const McpServerSnapshot& left, const McpServerSnapshot& right)
              {
                  return QString::compare(left.serverId, right.serverId,
                                          Qt::CaseSensitive) < 0;
              });
    return result;
}

bool McpServerRegistry::transition(const QString& serverId,
                                   McpServerState state, QString& errorMessage)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end())
    {
        errorMessage = QStringLiteral("Unknown MCP server: %1").arg(serverId);
        return false;
    }
    if (!transitionAllowed(iterator->state, state))
    {
        errorMessage = QStringLiteral(
                           "Invalid MCP server state transition: "
                           "%1 -> %2 for %3")
                           .arg(mcpServerStateName(iterator->state),
                                mcpServerStateName(state), serverId);
        return false;
    }
    iterator->state = state;
    return true;
}

bool McpServerRegistry::setInitialization(
    const QString& serverId, const McpInitializeResult& initialization)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end()) return false;
    iterator->protocolVersion = initialization.protocolVersion;
    iterator->capabilities = initialization.capabilities;
    iterator->serverInfo = initialization.serverInfo;
    iterator->instructions = initialization.instructions;
    ++iterator->capabilityRevision;
    return true;
}

bool McpServerRegistry::replaceTools(const QString& serverId,
                                     qsizetype toolCount)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end()) return false;
    iterator->toolCount = toolCount;
    ++iterator->capabilityRevision;
    return true;
}

bool McpServerRegistry::replaceResources(const QString& serverId,
                                         qsizetype resourceCount,
                                         qsizetype resourceTemplateCount)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end()) return false;
    iterator->resourceCount = resourceCount;
    iterator->resourceTemplateCount = resourceTemplateCount;
    ++iterator->capabilityRevision;
    return true;
}

bool McpServerRegistry::replacePrompts(const QString& serverId,
                                       qsizetype promptCount)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end()) return false;
    iterator->promptCount = promptCount;
    ++iterator->capabilityRevision;
    return true;
}

bool McpServerRegistry::replaceRoots(const QString& serverId,
                                     qsizetype rootCount)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end()) return false;
    iterator->rootCount = rootCount;
    ++iterator->capabilityRevision;
    return true;
}

bool McpServerRegistry::revokeCapabilities(const QString& serverId)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end()) return false;
    if (iterator->protocolVersion.isEmpty() &&
        iterator->capabilities.raw.isEmpty() &&
        iterator->serverInfo.isEmpty() && iterator->instructions.isEmpty() &&
        iterator->toolCount == 0 && iterator->resourceCount == 0 &&
        iterator->resourceTemplateCount == 0 && iterator->promptCount == 0)
        return true;
    iterator->protocolVersion.clear();
    iterator->capabilities = {};
    iterator->serverInfo = {};
    iterator->instructions.clear();
    iterator->toolCount = 0;
    iterator->resourceCount = 0;
    iterator->resourceTemplateCount = 0;
    iterator->promptCount = 0;
    ++iterator->capabilityRevision;
    return true;
}

bool McpServerRegistry::setError(const QString& serverId, const QString& code,
                                 const QString& message)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end()) return false;
    iterator->lastErrorCode = code;
    iterator->lastErrorMessage = message;
    return true;
}

bool McpServerRegistry::clearError(const QString& serverId)
{
    return setError(serverId, {}, {});
}
}  // namespace qtllm::infrastructure::mcp
