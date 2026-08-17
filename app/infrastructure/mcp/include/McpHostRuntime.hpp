#pragma once

#include "McpServerProcess.hpp"
#include "McpServerRegistry.hpp"
#include "McpTransport.hpp"
#include "ToolDefinition.hpp"
#include "ToolRegistry.hpp"
#include "ToolResult.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSharedPointer>

#include <functional>

namespace qtllm::infrastructure::mcp
{
class McpHostRuntime final : public QObject
{
    Q_OBJECT

   public:
    using TransportFactory = std::function<QSharedPointer<McpTransport>(
        const McpServerConfig& config)>;

    explicit McpHostRuntime(QObject* parent = nullptr);
    explicit McpHostRuntime(TransportFactory transportFactory,
                            QObject* parent = nullptr);
    ~McpHostRuntime() override;

    bool addServer(McpServerConfig config, QString& errorMessage);
    bool removeServer(const QString& serverId, QString& errorMessage);
    [[nodiscard]] QStringList serverIds() const;
    [[nodiscard]] McpTransport* transport(const QString& serverId) const;
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
        QList<agent::ToolDefinition> tools;
        QSet<QString> cursors;
    };
    struct ServerConnection
    {
        McpServerConfig config;
        QSharedPointer<McpTransport> transport;
        bool stopRequested = false;
        bool restartRequested = false;
    };
    struct NotificationWindow
    {
        qint64 startedAtMs = 0;
        int accepted = 0;
        bool reported = false;
    };

    void connectTransport(const QString& serverId, McpTransport* transport);
    void handleNotification(const QString& serverId, const QString& method,
                            const QJsonObject& params);
    QString invalidRequest(const QString& serverId, const QString& method,
                           const QString& message);
    void handleResponse(const QString& serverId, const QString& requestId,
                        const QString& method, const QJsonObject& result);
    void handleFailure(const QString& serverId, const QString& requestId,
                       const QString& method, const QString& code,
                       const QString& message,
                       const PendingRequest* knownPending = nullptr);
    bool changeState(const QString& serverId, McpServerState state);
    void publishSnapshot(const QString& serverId);
    void revokeCapabilities(const QString& serverId);
    void discardPendingRequests(const QString& serverId);
    [[nodiscard]] bool hasToolRefresh(const QString& serverId) const;
    void refreshQueuedTools(const QString& serverId);
    [[nodiscard]] bool acceptNotification(const QString& serverId);

    TransportFactory transportFactory_;
    QHash<QString, ServerConnection> connections_;
    QHash<QString, PendingRequest> pending_;
    QSet<QString> queuedToolRefreshes_;
    QElapsedTimer notificationClock_;
    QHash<QString, NotificationWindow> notificationWindows_;
    McpServerRegistry serverRegistry_;
    ToolRegistry toolRegistry_;
};
}  // namespace qtllm::infrastructure::mcp
