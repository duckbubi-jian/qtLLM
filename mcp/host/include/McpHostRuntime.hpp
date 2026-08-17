#pragma once

#include "McpCatalog.hpp"
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
    [[nodiscard]] QList<McpResourceDefinition> resources(
        const QString& serverId = {}) const;
    [[nodiscard]] QList<McpResourceTemplateDefinition> resourceTemplates(
        const QString& serverId = {}) const;
    [[nodiscard]] QList<McpPromptDefinition> prompts(
        const QString& serverId = {}) const;
    [[nodiscard]] QList<McpRoot> roots(const QString& serverId) const;
    [[nodiscard]] bool isResourceSubscribed(const QString& serverId,
                                            const QString& uri) const;
    [[nodiscard]] QString agentInstructions() const;
    [[nodiscard]] const ToolRegistry& registry() const;
    [[nodiscard]] std::optional<McpServerSnapshot> serverSnapshot(
        const QString& serverId) const;
    [[nodiscard]] QList<McpServerSnapshot> serverSnapshots() const;
    bool setUseInstructions(const QString& serverId, bool enabled,
                            QString& errorMessage);

    void startServer(const QString& serverId);
    void stopServer(const QString& serverId);
    QString initialize(const QString& serverId);
    QString ping(const QString& serverId);
    QString listTools(const QString& serverId);
    QString listResources(const QString& serverId);
    QString listResourceTemplates(const QString& serverId);
    QString readResource(const QString& serverId, const QString& uri);
    QString subscribeResource(const QString& serverId, const QString& uri);
    QString unsubscribeResource(const QString& serverId, const QString& uri);
    QString listPrompts(const QString& serverId);
    QString getPrompt(const QString& serverId, const QString& name,
                      const QJsonObject& arguments = {});
    QString completePrompt(const QString& serverId, const QString& promptName,
                           const QString& argumentName, const QString& value,
                           const QJsonObject& contextArguments = {});
    QString completeResourceTemplate(const QString& serverId,
                                     const QString& uriTemplate,
                                     const QString& argumentName,
                                     const QString& value,
                                     const QJsonObject& contextArguments = {});
    QString setLoggingLevel(const QString& serverId, const QString& level);
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
    void resourcesChanged(
        const QString& serverId,
        const QList<qtllm::infrastructure::mcp::McpResourceDefinition>&
            resources);
    void resourceTemplatesChanged(
        const QString& serverId,
        const QList<qtllm::infrastructure::mcp::McpResourceTemplateDefinition>&
            templates);
    void resourceReadReady(
        const qtllm::infrastructure::mcp::McpResourceReadResult& result);
    void resourceSubscriptionChanged(const QString& serverId,
                                     const QString& uri, bool subscribed);
    void resourceUpdated(const QString& serverId, const QString& uri);
    void promptsChanged(
        const QString& serverId,
        const QList<qtllm::infrastructure::mcp::McpPromptDefinition>& prompts);
    void promptReady(const qtllm::infrastructure::mcp::McpPromptResult& result);
    void completionReady(
        const qtllm::infrastructure::mcp::McpCompletionResult& result);
    void loggingLevelChanged(const QString& serverId, const QString& level);
    void rootsRequested(
        const QString& serverId,
        const QList<qtllm::infrastructure::mcp::McpRoot>& roots);
    void pingCompleted(const QString& serverId, const QString& requestId,
                       qint64 elapsedMs);
    void pingRequested(const QString& serverId);
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
        CallTool,
        ListResources,
        ListResourceTemplates,
        ReadResource,
        SubscribeResource,
        UnsubscribeResource,
        ListPrompts,
        GetPrompt,
        Complete,
        SetLoggingLevel,
        Ping
    };
    struct PendingRequest
    {
        QString serverId;
        Operation operation = Operation::Initialize;
        QString toolName;
        QList<agent::ToolDefinition> tools;
        QSet<QString> cursors;
        QList<McpResourceDefinition> resources;
        QList<McpResourceTemplateDefinition> resourceTemplates;
        QList<McpPromptDefinition> prompts;
        QString subject;
        QString referenceType;
        qint64 startedAtMs = 0;
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
    struct CatalogError
    {
        QString code;
        QString message;
    };

    void connectTransport(const QString& serverId, McpTransport* transport);
    void handleNotification(const QString& serverId, const QString& method,
                            const QJsonObject& params);
    void handleServerRequest(const QString& serverId,
                             const QJsonValue& requestId, const QString& method,
                             const QJsonObject& params);
    QString invalidRequest(const QString& serverId, const QString& method,
                           const QString& message);
    QString complete(const QString& serverId, const QString& referenceType,
                     const QString& reference, const QString& argumentName,
                     const QString& value, const QJsonObject& contextArguments);
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
    [[nodiscard]] bool hasPendingOperation(const QString& serverId,
                                           Operation operation) const;
    void refreshQueuedResources(const QString& serverId);
    void refreshQueuedPrompts(const QString& serverId);
    void recordCatalogFailure(const QString& serverId, Operation operation,
                              const QString& code, const QString& message);
    void recordCatalogSuccess(const QString& serverId, Operation operation);
    [[nodiscard]] bool acceptNotification(const QString& serverId);

    TransportFactory transportFactory_;
    QHash<QString, ServerConnection> connections_;
    QHash<QString, PendingRequest> pending_;
    QSet<QString> queuedToolRefreshes_;
    QSet<QString> queuedResourceRefreshes_;
    QSet<QString> queuedPromptRefreshes_;
    QHash<QString, QSet<QString>> resourceSubscriptions_;
    QHash<QString, QHash<int, CatalogError>> catalogErrors_;
    QElapsedTimer notificationClock_;
    QHash<QString, NotificationWindow> notificationWindows_;
    McpServerRegistry serverRegistry_;
    ToolRegistry toolRegistry_;
    McpResourceRegistry resourceRegistry_;
    McpPromptRegistry promptRegistry_;
    McpRootRegistry rootRegistry_;
};
}  // namespace qtllm::infrastructure::mcp
