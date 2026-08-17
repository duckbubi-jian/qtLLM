#pragma once

#include "McpProtocol.hpp"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QString>

#include <optional>

namespace qtllm::infrastructure::mcp
{
enum class McpServerState
{
    Stopped,
    Starting,
    Initializing,
    Ready,
    Degraded,
    Failed,
    Stopping
};

[[nodiscard]] QString mcpServerStateName(McpServerState state);

struct McpServerSnapshot
{
    QString serverId;
    McpServerState state = McpServerState::Stopped;
    quint64 capabilityRevision = 0;
    QString protocolVersion;
    McpServerCapabilities capabilities;
    QJsonObject serverInfo;
    QString instructions;
    qsizetype toolCount = 0;
    QString lastErrorCode;
    QString lastErrorMessage;
};

class McpServerRegistry final
{
   public:
    bool addServer(const QString& serverId, QString& errorMessage);
    bool removeServer(const QString& serverId);
    [[nodiscard]] bool contains(const QString& serverId) const;
    [[nodiscard]] QStringList serverIds() const;
    [[nodiscard]] std::optional<McpServerSnapshot> snapshot(
        const QString& serverId) const;
    [[nodiscard]] QList<McpServerSnapshot> snapshots() const;

    bool transition(const QString& serverId, McpServerState state,
                    QString& errorMessage);
    bool setInitialization(const QString& serverId,
                           const McpInitializeResult& initialization);
    bool replaceTools(const QString& serverId, qsizetype toolCount);
    bool revokeCapabilities(const QString& serverId);
    bool setError(const QString& serverId, const QString& code,
                  const QString& message);
    bool clearError(const QString& serverId);

   private:
    QHash<QString, McpServerSnapshot> servers_;
};
}  // namespace qtllm::infrastructure::mcp

Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpServerState)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpServerSnapshot)
