#pragma once

#include "McpHostRuntime.hpp"
#include "McpServerProcess.hpp"
#include "StdioMcpTransport.hpp"
#include "ToolDefinition.hpp"
#include "ToolRegistry.hpp"
#include "ToolResult.hpp"

#include <QList>
#include <QObject>

namespace qtllm::infrastructure::mcp
{
class McpClientManager final : public QObject
{
    Q_OBJECT

   public:
    explicit McpClientManager(QObject* parent = nullptr);
    ~McpClientManager() override;

    bool addServer(McpServerConfig config, QString& errorMessage);
    bool removeServer(const QString& serverId, QString& errorMessage);
    [[nodiscard]] QStringList serverIds() const;
    [[nodiscard]] StdioMcpTransport* transport(const QString& serverId) const;
    [[nodiscard]] QList<agent::ToolDefinition> tools() const;
    [[nodiscard]] QString agentInstructions() const;
    [[nodiscard]] const ToolRegistry& registry() const;
    [[nodiscard]] std::optional<McpServerSnapshot> serverSnapshot(
        const QString& serverId) const;
    [[nodiscard]] QList<McpServerSnapshot> serverSnapshots() const;

    void startServer(const QString& serverId);
    void stopServer(const QString& serverId);
    QString initialize(const QString& serverId);
    QString listTools(const QString& serverId);
    QString callTool(const QString& qualifiedToolName,
                     const QJsonObject& arguments);
    void cancel(const QString& requestId);
    void cancelAll();

   signals:
    void serverStateChanged(const QString& serverId,
                            qtllm::infrastructure::mcp::McpServerState state);
    void capabilitySnapshotChanged(
        const qtllm::infrastructure::mcp::McpServerSnapshot& snapshot);
    void serverStarted(const QString& serverId);
    void serverStopped(const QString& serverId);
    void serverInitialized(const QString& serverId,
                           const QJsonObject& serverInfo);
    void toolsChanged(const QString& serverId,
                      const QList<qtllm::agent::ToolDefinition>& tools);
    void toolResultReady(const qtllm::agent::ToolResult& result);
    void requestFailed(const QString& serverId, const QString& requestId,
                       const QString& method, const QString& code,
                       const QString& message);
    void notificationReceived(const QString& serverId, const QString& method,
                              const QJsonObject& params);
    void progressReceived(const QString& serverId, const QJsonValue& token,
                          double progress, double total,
                          const QString& message);
    void loggingMessageReceived(const QString& serverId, const QString& level,
                                const QString& logger, const QJsonValue& data);
    void diagnosticReceived(const QString& serverId, const QString& text);
    void serverError(const QString& serverId, const QString& code,
                     const QString& message);

   private:
    McpHostRuntime runtime_;
};
}  // namespace qtllm::infrastructure::mcp
