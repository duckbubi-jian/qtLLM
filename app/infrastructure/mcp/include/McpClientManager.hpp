#pragma once

#include "McpServerProcess.hpp"
#include "StdioMcpTransport.hpp"
#include "ToolDefinition.hpp"
#include "ToolRegistry.hpp"
#include "ToolResult.hpp"

#include <QHash>
#include <QList>
#include <QObject>

#include <QSharedPointer>

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

    void startServer(const QString& serverId);
    void stopServer(const QString& serverId);
    QString initialize(const QString& serverId);
    QString listTools(const QString& serverId);
    QString callTool(const QString& qualifiedToolName,
                     const QJsonObject& arguments);
    void cancel(const QString& requestId);
    void cancelAll();

   signals:
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
    void diagnosticReceived(const QString& serverId, const QString& text);
    void serverError(const QString& serverId, const QString& code,
                     const QString& message);

   private:
    enum class Operation
    {
        Initialize,
        ListTools,
        CallTool
    };
    struct PendingRequest
    {
        QString serverId;
        Operation operation = Operation::Initialize;
        QString toolName;
    };
    struct Server
    {
        QSharedPointer<StdioMcpTransport> transport;
        QList<agent::ToolDefinition> tools;
        QString instructions;
        bool initialized = false;
    };

    void connectTransport(const QString& serverId,
                          StdioMcpTransport* transport);
    QString invalidRequest(const QString& serverId, const QString& method,
                           const QString& message);
    void handleResponse(const QString& serverId, const QString& requestId,
                        const QString& method, const QJsonObject& result);
    void handleFailure(const QString& serverId, const QString& requestId,
                       const QString& method, const QString& code,
                       const QString& message);

    QHash<QString, Server> servers_;
    QHash<QString, PendingRequest> pending_;
    ToolRegistry registry_;
};
}  // namespace qtllm::infrastructure::mcp
