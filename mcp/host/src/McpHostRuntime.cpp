#include "McpHostRuntime.hpp"

#include "McpProtocol.hpp"
#include "StdioMcpTransport.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace qtllm::infrastructure::mcp
{
namespace
{
constexpr auto clientName = "qtLLM";
constexpr auto clientVersion = "0.2.0";
constexpr qsizetype maximumServerInstructions = 4'096;
constexpr qsizetype maximumCombinedInstructions = 8'192;
constexpr auto notificationWindowMs = 1'000;
constexpr auto maximumNotificationsPerWindow = 100;

QJsonValue structuredToolResult(const QJsonObject& result)
{
    return result.value(QStringLiteral("structuredContent"));
}

QString textToolResult(const QJsonObject& result)
{
    QStringList text;
    for (const auto& value : result.value(QStringLiteral("content")).toArray())
    {
        const auto item = value.toObject();
        if (item.value(QStringLiteral("type")).toString() ==
            QLatin1String("text"))
            text.append(item.value(QStringLiteral("text")).toString());
    }
    return text.join(QLatin1Char('\n')).trimmed();
}

bool validateServerConfig(const McpServerConfig& config, QString& errorMessage)
{
    if (config.serverId.isEmpty())
    {
        errorMessage = QStringLiteral("MCP serverId must not be empty.");
        return false;
    }
    static const QRegularExpression validServerId(
        QStringLiteral("^[A-Za-z0-9_-]+$"));
    if (!validServerId.match(config.serverId).hasMatch())
    {
        errorMessage = QStringLiteral(
            "MCP serverId may contain letters, numbers, '-' and '_' only.");
        return false;
    }
    if (config.program.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("MCP server program must not be empty.");
        return false;
    }
    if (!QFileInfo(config.program).isAbsolute())
    {
        errorMessage =
            QStringLiteral("MCP server program must be an absolute path.");
        return false;
    }
    if (!config.workingDirectory.isEmpty() &&
        !QFileInfo(config.workingDirectory).isAbsolute())
    {
        errorMessage =
            QStringLiteral("MCP workingDirectory must be an absolute path.");
        return false;
    }
    if (!config.loggingLevel.isEmpty() &&
        !isSupportedLoggingLevel(config.loggingLevel))
    {
        errorMessage = QStringLiteral("Invalid MCP logging level.");
        return false;
    }
    return true;
}

bool parseToolPage(const QString& serverId, const QStringList& allowlist,
                   const QJsonObject& result,
                   const QList<agent::ToolDefinition>& accumulated,
                   QList<agent::ToolDefinition>& definitions,
                   QString& errorMessage)
{
    const auto toolsValue = result.value(QStringLiteral("tools"));
    if (!toolsValue.isArray())
    {
        errorMessage = QStringLiteral("tools/list result has no tools array.");
        return false;
    }

    QSet<QString> names;
    for (const auto& definition : accumulated)
        names.insert(definition.name);
    definitions = accumulated;
    for (const auto& value : toolsValue.toArray())
    {
        if (!value.isObject())
        {
            errorMessage =
                QStringLiteral("tools/list contains a non-object tool.");
            return false;
        }
        const auto object = value.toObject();
        const auto nameValue = object.value(QStringLiteral("name"));
        const auto descriptionValue =
            object.value(QStringLiteral("description"));
        const auto inputSchema = object.value(QStringLiteral("inputSchema"));
        const auto outputSchema = object.value(QStringLiteral("outputSchema"));
        const auto annotations = object.value(QStringLiteral("annotations"));
        const auto name = nameValue.toString().trimmed();
        if (!nameValue.isString() || name.isEmpty())
        {
            errorMessage = QStringLiteral(
                "tools/list contains a tool with an invalid name.");
            return false;
        }
        if (names.contains(name))
        {
            errorMessage =
                QStringLiteral("Duplicate tool: %1.%2").arg(serverId, name);
            return false;
        }
        names.insert(name);
        if (!descriptionValue.isUndefined() && !descriptionValue.isString())
        {
            errorMessage =
                QStringLiteral("Tool %1.%2 has a non-string description.")
                    .arg(serverId, name);
            return false;
        }
        if (!inputSchema.isObject())
        {
            errorMessage =
                QStringLiteral("Tool %1.%2 has no inputSchema object.")
                    .arg(serverId, name);
            return false;
        }
        if (!outputSchema.isUndefined() && !outputSchema.isObject())
        {
            errorMessage =
                QStringLiteral("Tool %1.%2 has an invalid outputSchema.")
                    .arg(serverId, name);
            return false;
        }
        if (!annotations.isUndefined() && !annotations.isObject())
        {
            errorMessage = QStringLiteral("Tool %1.%2 has invalid annotations.")
                               .arg(serverId, name);
            return false;
        }
        if (!allowlist.isEmpty() && !allowlist.contains(name)) continue;
        definitions.append({serverId + QLatin1Char('.') + name, serverId, name,
                            descriptionValue.toString(), inputSchema.toObject(),
                            outputSchema.toObject(), annotations.toObject(),
                            !outputSchema.isUndefined()});
    }
    return true;
}

agent::ToolFailureKind failureKind(const QString& code)
{
    if (code == QLatin1String("invalid_response") ||
        code == QLatin1String("invalid_message"))
        return agent::ToolFailureKind::Protocol;
    if (code == QLatin1String("remote_error"))
        return agent::ToolFailureKind::Server;
    if (code == QLatin1String("invalid_request"))
        return agent::ToolFailureKind::LocalValidation;
    return agent::ToolFailureKind::Transport;
}

agent::ToolOutcome failureOutcome(const QString& code)
{
    if (code == QLatin1String("cancelled") || code == QLatin1String("canceled"))
        return agent::ToolOutcome::Cancelled;
    switch (failureKind(code))
    {
        case agent::ToolFailureKind::LocalValidation:
            return agent::ToolOutcome::ValidationFailed;
        case agent::ToolFailureKind::Authorization:
            return agent::ToolOutcome::Denied;
        case agent::ToolFailureKind::Transport:
            return agent::ToolOutcome::TransportFailed;
        case agent::ToolFailureKind::Protocol:
            return agent::ToolOutcome::ProtocolFailed;
        case agent::ToolFailureKind::Server:
            return agent::ToolOutcome::ServerFailed;
        case agent::ToolFailureKind::Tool:
        case agent::ToolFailureKind::None:
            return agent::ToolOutcome::ToolFailed;
    }
    return agent::ToolOutcome::ToolFailed;
}
}  // namespace

McpHostRuntime::McpHostRuntime(QObject* parent)
    : McpHostRuntime(
          [](const McpServerConfig& config) -> QSharedPointer<McpTransport>
          { return QSharedPointer<StdioMcpTransport>::create(config); }, parent)
{
}

McpHostRuntime::McpHostRuntime(TransportFactory transportFactory,
                               QObject* parent)
    : QObject(parent), transportFactory_(std::move(transportFactory))
{
    qRegisterMetaType<agent::ToolDefinition>();
    qRegisterMetaType<agent::ToolResult>();
    qRegisterMetaType<agent::ToolFailureKind>();
    qRegisterMetaType<agent::ToolOutcome>();
    qRegisterMetaType<agent::ToolSideEffectState>();
    qRegisterMetaType<McpServerState>();
    qRegisterMetaType<McpServerSnapshot>();
    qRegisterMetaType<McpResourceDefinition>();
    qRegisterMetaType<McpResourceTemplateDefinition>();
    qRegisterMetaType<McpResourceReadResult>();
    qRegisterMetaType<McpPromptDefinition>();
    qRegisterMetaType<McpPromptResult>();
    qRegisterMetaType<McpCompletionResult>();
    qRegisterMetaType<McpRoot>();
    notificationClock_.start();
}

McpHostRuntime::~McpHostRuntime()
{
    for (auto& connection : connections_)
    {
        disconnect(connection.transport.get(), nullptr, this, nullptr);
        connection.transport->stop();
    }
    pending_.clear();
}

bool McpHostRuntime::addServer(McpServerConfig config, QString& errorMessage)
{
    config.serverId = config.serverId.trimmed();
    if (!validateServerConfig(config, errorMessage)) return false;
    if (connections_.contains(config.serverId))
    {
        errorMessage = QStringLiteral("MCP serverId is already registered: %1")
                           .arg(config.serverId);
        return false;
    }
    if (!transportFactory_)
    {
        errorMessage = QStringLiteral("MCP transport factory is unavailable.");
        return false;
    }
    auto transport = transportFactory_(config);
    if (transport.isNull())
    {
        errorMessage = QStringLiteral("MCP transport could not be created.");
        return false;
    }
    if (!serverRegistry_.addServer(config.serverId, errorMessage)) return false;
    if (!rootRegistry_.replaceServerRoots(config.serverId,
                                          config.authorizedRoots, errorMessage))
    {
        serverRegistry_.removeServer(config.serverId);
        return false;
    }
    serverRegistry_.replaceRoots(config.serverId,
                                 rootRegistry_.roots(config.serverId).size());

    const auto serverId = config.serverId;
    auto* transportPointer = transport.get();
    connections_.insert(
        serverId, {std::move(config), std::move(transport), false, false});
    connectTransport(serverId, transportPointer);
    emit serverStateChanged(serverId, McpServerState::Stopped);
    return true;
}

bool McpHostRuntime::removeServer(const QString& serverId,
                                  QString& errorMessage)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
    {
        errorMessage = QStringLiteral("Unknown MCP server: %1").arg(serverId);
        return false;
    }

    iterator->stopRequested = true;
    iterator->transport->cancelAll();
    if (iterator->transport->isRunning())
    {
        changeState(serverId, McpServerState::Stopping);
        iterator->transport->stop();
    }
    else
    {
        revokeCapabilities(serverId);
    }
    disconnect(iterator->transport.get(), nullptr, this, nullptr);
    for (auto pending = pending_.begin(); pending != pending_.end();)
    {
        if (pending->serverId == serverId)
            pending = pending_.erase(pending);
        else
            ++pending;
    }
    toolRegistry_.removeServer(serverId);
    resourceRegistry_.removeServer(serverId);
    promptRegistry_.removeServer(serverId);
    rootRegistry_.removeServer(serverId);
    resourceSubscriptions_.remove(serverId);
    serverRegistry_.removeServer(serverId);
    connections_.erase(iterator);
    return true;
}

QStringList McpHostRuntime::serverIds() const
{
    return serverRegistry_.serverIds();
}

McpTransport* McpHostRuntime::transport(const QString& serverId) const
{
    const auto iterator = connections_.constFind(serverId);
    return iterator == connections_.constEnd() ? nullptr
                                               : iterator->transport.get();
}

QList<agent::ToolDefinition> McpHostRuntime::tools() const
{
    return toolRegistry_.tools();
}

QList<McpResourceDefinition> McpHostRuntime::resources(
    const QString& serverId) const
{
    return resourceRegistry_.resources(serverId);
}

QList<McpResourceTemplateDefinition> McpHostRuntime::resourceTemplates(
    const QString& serverId) const
{
    return resourceRegistry_.templates(serverId);
}

QList<McpPromptDefinition> McpHostRuntime::prompts(
    const QString& serverId) const
{
    return promptRegistry_.prompts(serverId);
}

QList<McpRoot> McpHostRuntime::roots(const QString& serverId) const
{
    return rootRegistry_.roots(serverId);
}

bool McpHostRuntime::isResourceSubscribed(const QString& serverId,
                                          const QString& uri) const
{
    return resourceSubscriptions_.value(serverId).contains(uri);
}

QString McpHostRuntime::agentInstructions() const
{
    QString combined;
    for (const auto& snapshot : serverRegistry_.snapshots())
    {
        const auto connection = connections_.constFind(snapshot.serverId);
        if ((snapshot.state != McpServerState::Ready &&
             snapshot.state != McpServerState::Degraded) ||
            connection == connections_.constEnd() ||
            !connection->config.useInstructions ||
            snapshot.instructions.trimmed().isEmpty())
            continue;
        const auto prefix = snapshot.serverId + QStringLiteral(": ");
        const auto separatorCharacters = combined.isEmpty() ? 0 : 1;
        const auto remaining = maximumCombinedInstructions - combined.size() -
                               separatorCharacters - prefix.size();
        if (remaining <= 0) break;
        if (!combined.isEmpty()) combined.append(QLatin1Char('\n'));
        combined.append(prefix);
        combined.append(snapshot.instructions.trimmed().left(
            qMin(maximumServerInstructions, remaining)));
    }
    return combined;
}

const ToolRegistry& McpHostRuntime::registry() const
{
    return toolRegistry_;
}

std::optional<McpServerSnapshot> McpHostRuntime::serverSnapshot(
    const QString& serverId) const
{
    return serverRegistry_.snapshot(serverId);
}

QList<McpServerSnapshot> McpHostRuntime::serverSnapshots() const
{
    return serverRegistry_.snapshots();
}

bool McpHostRuntime::setUseInstructions(const QString& serverId, bool enabled,
                                        QString& errorMessage)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
    {
        errorMessage = QStringLiteral("Unknown MCP server: %1").arg(serverId);
        return false;
    }
    iterator->config.useInstructions = enabled;
    errorMessage.clear();
    return true;
}

void McpHostRuntime::startServer(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
    {
        emit serverError(serverId, QStringLiteral("unknown_server"),
                         QStringLiteral("Unknown MCP server."));
        return;
    }
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Stopped &&
                      snapshot->state != McpServerState::Failed))
    {
        emit serverError(serverId, QStringLiteral("invalid_state"),
                         QStringLiteral("MCP server cannot be started from "
                                        "its current state."));
        return;
    }

    if (snapshot->state == McpServerState::Failed &&
        iterator->transport->isRunning())
    {
        iterator->restartRequested = true;
        iterator->stopRequested = true;
        if (!changeState(serverId, McpServerState::Stopping))
        {
            iterator->restartRequested = false;
            iterator->stopRequested = false;
            return;
        }
        iterator->transport->cancelAll();
        iterator->transport->stop();
        return;
    }

    revokeCapabilities(serverId);
    catalogErrors_.remove(serverId);
    serverRegistry_.clearError(serverId);
    iterator->stopRequested = false;
    iterator->restartRequested = false;
    if (!changeState(serverId, McpServerState::Starting)) return;
    iterator->transport->start();
}

void McpHostRuntime::stopServer(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
    {
        emit serverError(serverId, QStringLiteral("unknown_server"),
                         QStringLiteral("Unknown MCP server."));
        return;
    }
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (snapshot && snapshot->state == McpServerState::Stopped) return;

    iterator->stopRequested = true;
    iterator->restartRequested = false;
    if (!iterator->transport->isRunning())
    {
        if (!changeState(serverId, McpServerState::Stopping)) return;
        revokeCapabilities(serverId);
        changeState(serverId, McpServerState::Stopped);
        emit serverStopped(serverId);
        iterator->stopRequested = false;
        return;
    }
    if (!changeState(serverId, McpServerState::Stopping)) return;
    iterator->transport->cancelAll();
    iterator->transport->stop();
}

QString McpHostRuntime::initialize(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("initialize"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || snapshot->state != McpServerState::Starting)
        return invalidRequest(
            serverId, QStringLiteral("initialize"),
            QStringLiteral("MCP server is not ready to initialize."));
    if (!iterator->transport->isRunning())
        return invalidRequest(serverId, QStringLiteral("initialize"),
                              QStringLiteral("MCP server is not running."));
    if (!changeState(serverId, McpServerState::Initializing)) return {};

    const auto requestId = iterator->transport->request(
        QStringLiteral("initialize"),
        {{QStringLiteral("protocolVersion"), latestSupportedProtocolVersion()},
         {QStringLiteral("capabilities"),
          QJsonObject{{QStringLiteral("roots"),
                       QJsonObject{{QStringLiteral("listChanged"), false}}}}},
         {QStringLiteral("clientInfo"),
          QJsonObject{{QStringLiteral("name"), QString::fromLatin1(clientName)},
                      {QStringLiteral("version"),
                       QString::fromLatin1(clientVersion)}}}},
        iterator->config.initializeTimeoutMs);
    if (requestId.isEmpty())
    {
        changeState(serverId, McpServerState::Failed);
        return {};
    }
    pending_.insert(requestId, {serverId, Operation::Initialize, {}});
    return requestId;
}

QString McpHostRuntime::ping(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("ping"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("ping"),
                              QStringLiteral("MCP server is not ready."));
    if (hasPendingOperation(serverId, Operation::Ping))
        return invalidRequest(serverId, QStringLiteral("ping"),
                              QStringLiteral("MCP ping is already pending."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("ping"), {}, iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::Ping;
        pending.startedAtMs = notificationClock_.elapsed();
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::listTools(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("tools/list"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("tools/list"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.tools)
        return invalidRequest(
            serverId, QStringLiteral("tools/list"),
            QStringLiteral("MCP server did not declare the Tools capability."));
    if (hasToolRefresh(serverId))
        return invalidRequest(
            serverId, QStringLiteral("tools/list"),
            QStringLiteral("MCP tool refresh is already in progress."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("tools/list"), {}, iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::ListTools;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::listResources(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("resources/list"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("resources/list"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.resources)
        return invalidRequest(
            serverId, QStringLiteral("resources/list"),
            QStringLiteral("MCP server did not declare Resources."));
    if (hasPendingOperation(serverId, Operation::ListResources))
        return invalidRequest(
            serverId, QStringLiteral("resources/list"),
            QStringLiteral("MCP resource refresh is already in progress."));
    const auto requestId =
        iterator->transport->request(QStringLiteral("resources/list"), {},
                                     iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::ListResources;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::listResourceTemplates(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId,
                              QStringLiteral("resources/templates/list"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId,
                              QStringLiteral("resources/templates/list"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.resources)
        return invalidRequest(
            serverId, QStringLiteral("resources/templates/list"),
            QStringLiteral("MCP server did not declare Resources."));
    if (hasPendingOperation(serverId, Operation::ListResourceTemplates))
        return invalidRequest(
            serverId, QStringLiteral("resources/templates/list"),
            QStringLiteral(
                "MCP resource template refresh is already in progress."));
    const auto requestId =
        iterator->transport->request(QStringLiteral("resources/templates/list"),
                                     {}, iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::ListResourceTemplates;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::readResource(const QString& serverId,
                                     const QString& uri)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("resources/read"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("resources/read"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.resources)
        return invalidRequest(
            serverId, QStringLiteral("resources/read"),
            QStringLiteral("MCP server did not declare Resources."));
    if (resourceRegistry_.find(serverId, uri) == nullptr)
        return invalidRequest(serverId, QStringLiteral("resources/read"),
                              QStringLiteral("Resource is not registered."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("resources/read"), {{QStringLiteral("uri"), uri}},
        iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::ReadResource;
        pending.subject = uri;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::subscribeResource(const QString& serverId,
                                          const QString& uri)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("resources/subscribe"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("resources/subscribe"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.resourcesSubscribe)
        return invalidRequest(
            serverId, QStringLiteral("resources/subscribe"),
            QStringLiteral("MCP server did not declare subscriptions."));
    if (resourceRegistry_.find(serverId, uri) == nullptr)
        return invalidRequest(serverId, QStringLiteral("resources/subscribe"),
                              QStringLiteral("Resource is not registered."));
    if (resourceSubscriptions_.value(serverId).contains(uri))
        return invalidRequest(
            serverId, QStringLiteral("resources/subscribe"),
            QStringLiteral("Resource is already subscribed."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("resources/subscribe"), {{QStringLiteral("uri"), uri}},
        iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::SubscribeResource;
        pending.subject = uri;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::unsubscribeResource(const QString& serverId,
                                            const QString& uri)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("resources/unsubscribe"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("resources/unsubscribe"),
                              QStringLiteral("MCP server is not ready."));
    if (!resourceSubscriptions_.value(serverId).contains(uri))
        return invalidRequest(serverId, QStringLiteral("resources/unsubscribe"),
                              QStringLiteral("Resource is not subscribed."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("resources/unsubscribe"), {{QStringLiteral("uri"), uri}},
        iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::UnsubscribeResource;
        pending.subject = uri;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::listPrompts(const QString& serverId)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("prompts/list"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("prompts/list"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.prompts)
        return invalidRequest(
            serverId, QStringLiteral("prompts/list"),
            QStringLiteral("MCP server did not declare Prompts."));
    if (hasPendingOperation(serverId, Operation::ListPrompts))
        return invalidRequest(
            serverId, QStringLiteral("prompts/list"),
            QStringLiteral("MCP prompt refresh is already in progress."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("prompts/list"), {}, iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::ListPrompts;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::getPrompt(const QString& serverId, const QString& name,
                                  const QJsonObject& arguments)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("prompts/get"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("prompts/get"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.prompts)
        return invalidRequest(
            serverId, QStringLiteral("prompts/get"),
            QStringLiteral("MCP server did not declare Prompts."));
    QString validationError;
    if (!promptRegistry_.validateArguments(serverId, name, arguments,
                                           validationError))
        return invalidRequest(serverId, QStringLiteral("prompts/get"),
                              validationError);
    const auto requestId =
        iterator->transport->request(QStringLiteral("prompts/get"),
                                     {{QStringLiteral("name"), name},
                                      {QStringLiteral("arguments"), arguments}},
                                     iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::GetPrompt;
        pending.subject = name;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::completePrompt(const QString& serverId,
                                       const QString& promptName,
                                       const QString& argumentName,
                                       const QString& value,
                                       const QJsonObject& contextArguments)
{
    return complete(serverId, QStringLiteral("ref/prompt"), promptName,
                    argumentName, value, contextArguments);
}

QString McpHostRuntime::completeResourceTemplate(
    const QString& serverId, const QString& uriTemplate,
    const QString& argumentName, const QString& value,
    const QJsonObject& contextArguments)
{
    return complete(serverId, QStringLiteral("ref/resource"), uriTemplate,
                    argumentName, value, contextArguments);
}

QString McpHostRuntime::complete(const QString& serverId,
                                 const QString& referenceType,
                                 const QString& reference,
                                 const QString& argumentName,
                                 const QString& value,
                                 const QJsonObject& contextArguments)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("completion/complete"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("completion/complete"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.completions)
        return invalidRequest(
            serverId, QStringLiteral("completion/complete"),
            QStringLiteral("MCP server did not declare Completions."));
    if (argumentName.trimmed().isEmpty() || argumentName.size() > 256 ||
        value.size() > 4'096 || contextArguments.size() > 64)
        return invalidRequest(
            serverId, QStringLiteral("completion/complete"),
            QStringLiteral("MCP completion arguments are invalid."));
    for (auto argument = contextArguments.constBegin();
         argument != contextArguments.constEnd(); ++argument)
        if (!argument->isString() || argument.key().trimmed().isEmpty() ||
            argument.key().size() > 256 || argument->toString().size() > 4'096)
            return invalidRequest(
                serverId, QStringLiteral("completion/complete"),
                QStringLiteral("MCP completion context is invalid."));

    QJsonObject referenceObject{{QStringLiteral("type"), referenceType}};
    if (referenceType == QLatin1String("ref/prompt"))
    {
        const auto* prompt = promptRegistry_.find(serverId, reference);
        if (prompt == nullptr)
            return invalidRequest(
                serverId, QStringLiteral("completion/complete"),
                QStringLiteral("Prompt is not registered: %1.%2")
                    .arg(serverId, reference));
        const auto knownArgument =
            std::any_of(prompt->arguments.cbegin(), prompt->arguments.cend(),
                        [&argumentName](const auto& argument)
                        { return argument.name == argumentName; });
        if (!knownArgument)
            return invalidRequest(
                serverId, QStringLiteral("completion/complete"),
                QStringLiteral("Prompt argument is not registered: %1")
                    .arg(argumentName));
        referenceObject.insert(QStringLiteral("name"), reference);
    }
    else if (referenceType == QLatin1String("ref/resource"))
    {
        if (resourceRegistry_.findTemplate(serverId, reference) == nullptr)
            return invalidRequest(
                serverId, QStringLiteral("completion/complete"),
                QStringLiteral("Resource template is not registered."));
        referenceObject.insert(QStringLiteral("uri"), reference);
    }
    else
    {
        return invalidRequest(serverId, QStringLiteral("completion/complete"),
                              QStringLiteral("Unknown completion reference."));
    }

    const auto requestId = iterator->transport->request(
        QStringLiteral("completion/complete"),
        {{QStringLiteral("ref"), referenceObject},
         {QStringLiteral("argument"),
          QJsonObject{{QStringLiteral("name"), argumentName},
                      {QStringLiteral("value"), value}}},
         {QStringLiteral("context"),
          QJsonObject{{QStringLiteral("arguments"), contextArguments}}}},
        iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::Complete;
        pending.subject = reference;
        pending.toolName = argumentName;
        pending.referenceType = referenceType;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::setLoggingLevel(const QString& serverId,
                                        const QString& level)
{
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("logging/setLevel"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("logging/setLevel"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.logging)
        return invalidRequest(
            serverId, QStringLiteral("logging/setLevel"),
            QStringLiteral("MCP server did not declare Logging."));
    if (!isSupportedLoggingLevel(level))
        return invalidRequest(serverId, QStringLiteral("logging/setLevel"),
                              QStringLiteral("Invalid MCP logging level."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("logging/setLevel"), {{QStringLiteral("level"), level}},
        iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
    {
        PendingRequest pending;
        pending.serverId = serverId;
        pending.operation = Operation::SetLoggingLevel;
        pending.subject = level;
        pending_.insert(requestId, std::move(pending));
    }
    return requestId;
}

QString McpHostRuntime::callTool(const QString& qualifiedToolName,
                                 const QJsonObject& arguments)
{
    const auto separator = qualifiedToolName.indexOf(QLatin1Char('.'));
    if (separator <= 0 || separator == qualifiedToolName.size() - 1)
        return invalidRequest(
            {}, QStringLiteral("tools/call"),
            QStringLiteral("Tool name must be serverId.name."));
    const auto serverId = qualifiedToolName.left(separator);
    const auto toolName = qualifiedToolName.mid(separator + 1);
    const auto iterator = connections_.find(serverId);
    if (iterator == connections_.end())
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              QStringLiteral("Unknown MCP server."));
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              QStringLiteral("MCP server is not ready."));
    if (!snapshot->capabilities.tools)
        return invalidRequest(
            serverId, QStringLiteral("tools/call"),
            QStringLiteral("MCP server did not declare the Tools capability."));
    if (toolRegistry_.find(qualifiedToolName) == nullptr)
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              QStringLiteral("Tool is not registered: %1")
                                  .arg(qualifiedToolName));
    QString validationError;
    if (!toolRegistry_.validateArguments(qualifiedToolName, arguments,
                                         validationError))
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              validationError);

    const auto requestId =
        iterator->transport->request(QStringLiteral("tools/call"),
                                     {{QStringLiteral("name"), toolName},
                                      {QStringLiteral("arguments"), arguments}},
                                     iterator->config.requestTimeoutMs);
    if (!requestId.isEmpty())
        pending_.insert(requestId, {serverId, Operation::CallTool, toolName});
    return requestId;
}

void McpHostRuntime::cancel(const QString& requestId)
{
    const auto iterator = pending_.find(requestId);
    if (iterator == pending_.end()) return;
    if (auto* serverTransport = transport(iterator->serverId))
        serverTransport->cancel(requestId);
}

void McpHostRuntime::cancelAll()
{
    for (auto& connection : connections_)
        connection.transport->cancelAll();
}

void McpHostRuntime::connectTransport(const QString& serverId,
                                      McpTransport* transportPointer)
{
    connect(transportPointer, &McpTransport::started, this,
            [this, serverId] { emit serverStarted(serverId); });
    connect(transportPointer, &McpTransport::stopped, this,
            [this, serverId]
            {
                const auto iterator = connections_.find(serverId);
                if (iterator == connections_.end()) return;
                const auto requested = iterator->stopRequested;
                const auto restart = iterator->restartRequested;
                discardPendingRequests(serverId);
                revokeCapabilities(serverId);
                changeState(serverId, requested ? McpServerState::Stopped
                                                : McpServerState::Failed);
                iterator->stopRequested = false;
                iterator->restartRequested = false;
                emit serverStopped(serverId);
                if (restart)
                    QMetaObject::invokeMethod(
                        this, [this, serverId] { startServer(serverId); },
                        Qt::QueuedConnection);
            });
    connect(transportPointer, &McpTransport::responseReceived, this,
            [this, serverId](const QString& requestId, const QString& method,
                             const QJsonObject& result)
            { handleResponse(serverId, requestId, method, result); });
    connect(transportPointer, &McpTransport::requestFailed, this,
            [this, serverId](const QString& requestId, const QString& method,
                             const QString& code, const QString& message)
            { handleFailure(serverId, requestId, method, code, message); });
    connect(transportPointer, &McpTransport::notificationReceived, this,
            [this, serverId](const QString& method, const QJsonObject& params)
            { handleNotification(serverId, method, params); });
    connect(transportPointer, &McpTransport::requestReceived, this,
            [this, serverId](const QJsonValue& requestId, const QString& method,
                             const QJsonObject& params)
            { handleServerRequest(serverId, requestId, method, params); });
    connect(transportPointer, &McpTransport::diagnosticReceived, this,
            [this, serverId](const QString& text)
            { emit diagnosticReceived(serverId, text); });
    connect(transportPointer, &McpTransport::transportError, this,
            [this, serverId](const QString& code, const QString& message)
            {
                if (!connections_.contains(serverId)) return;
                discardPendingRequests(serverId);
                revokeCapabilities(serverId);
                catalogErrors_.remove(serverId);
                serverRegistry_.setError(serverId, code, message);
                changeState(serverId, McpServerState::Failed);
                publishSnapshot(serverId);
                emit serverError(serverId, code, message);
            });
}

void McpHostRuntime::handleNotification(const QString& serverId,
                                        const QString& method,
                                        const QJsonObject& params)
{
    if (method == QLatin1String("notifications/tools/list_changed"))
    {
        const auto snapshot = serverRegistry_.snapshot(serverId);
        if (snapshot && snapshot->capabilities.toolsListChanged &&
            (snapshot->state == McpServerState::Ready ||
             snapshot->state == McpServerState::Degraded))
        {
            if (hasToolRefresh(serverId))
                queuedToolRefreshes_.insert(serverId);
            else
                listTools(serverId);
        }
        emit notificationReceived(serverId, method, params);
        return;
    }
    if (method == QLatin1String("notifications/resources/list_changed"))
    {
        const auto snapshot = serverRegistry_.snapshot(serverId);
        if (snapshot && snapshot->capabilities.resourcesListChanged &&
            (snapshot->state == McpServerState::Ready ||
             snapshot->state == McpServerState::Degraded))
        {
            if (hasPendingOperation(serverId, Operation::ListResources) ||
                hasPendingOperation(serverId, Operation::ListResourceTemplates))
                queuedResourceRefreshes_.insert(serverId);
            else
            {
                listResources(serverId);
                listResourceTemplates(serverId);
            }
        }
        emit notificationReceived(serverId, method, params);
        return;
    }
    if (method == QLatin1String("notifications/prompts/list_changed"))
    {
        const auto snapshot = serverRegistry_.snapshot(serverId);
        if (snapshot && snapshot->capabilities.promptsListChanged &&
            (snapshot->state == McpServerState::Ready ||
             snapshot->state == McpServerState::Degraded))
        {
            if (hasPendingOperation(serverId, Operation::ListPrompts))
                queuedPromptRefreshes_.insert(serverId);
            else
                listPrompts(serverId);
        }
        emit notificationReceived(serverId, method, params);
        return;
    }
    if (!acceptNotification(serverId)) return;

    if (method == QLatin1String("notifications/progress"))
    {
        const auto token = params.value(QStringLiteral("progressToken"));
        const auto progress = params.value(QStringLiteral("progress"));
        const auto total = params.value(QStringLiteral("total"));
        const auto message = params.value(QStringLiteral("message"));
        if ((!token.isString() && !token.isDouble()) || !progress.isDouble() ||
            (!total.isUndefined() && !total.isDouble()) ||
            (!message.isUndefined() && !message.isString()))
        {
            emit serverError(
                serverId, QStringLiteral("invalid_notification"),
                QStringLiteral("Invalid MCP progress notification."));
        }
        else
        {
            emit progressReceived(serverId, token, progress.toDouble(),
                                  total.isDouble() ? total.toDouble() : -1.0,
                                  message.toString());
        }
    }
    else if (method == QLatin1String("notifications/message"))
    {
        const auto level = params.value(QStringLiteral("level"));
        const auto logger = params.value(QStringLiteral("logger"));
        if (!level.isString() ||
            (!logger.isUndefined() && !logger.isString()) ||
            !params.contains(QStringLiteral("data")))
        {
            emit serverError(
                serverId, QStringLiteral("invalid_notification"),
                QStringLiteral("Invalid MCP logging notification."));
        }
        else
        {
            emit loggingMessageReceived(serverId, level.toString(),
                                        logger.toString(),
                                        params.value(QStringLiteral("data")));
        }
    }
    else if (method == QLatin1String("notifications/resources/updated"))
    {
        const auto uri = params.value(QStringLiteral("uri"));
        if (!uri.isString() || uri.toString().trimmed().isEmpty() ||
            !resourceSubscriptions_.value(serverId).contains(uri.toString()))
            emit serverError(
                serverId, QStringLiteral("invalid_notification"),
                QStringLiteral("Invalid or unsolicited resource update."));
        else
            emit resourceUpdated(serverId, uri.toString());
    }
    emit notificationReceived(serverId, method, params);
}

void McpHostRuntime::handleServerRequest(const QString& serverId,
                                         const QJsonValue& requestId,
                                         const QString& method,
                                         const QJsonObject&)
{
    auto* serverTransport = transport(serverId);
    if (serverTransport == nullptr) return;
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (!snapshot || (snapshot->state != McpServerState::Ready &&
                      snapshot->state != McpServerState::Degraded))
    {
        serverTransport->respondError(
            requestId, -32002,
            QStringLiteral("MCP Host is not ready for Server requests."));
        return;
    }
    if (method == QLatin1String("ping"))
    {
        serverTransport->respond(requestId, {});
        emit pingRequested(serverId);
        return;
    }
    if (method != QLatin1String("roots/list"))
    {
        serverTransport->respondError(requestId, -32601,
                                      QStringLiteral("Method not found."));
        return;
    }

    const auto configuredRoots = rootRegistry_.roots(serverId);
    QJsonArray roots;
    for (const auto& root : configuredRoots)
        roots.append(QJsonObject{{QStringLiteral("uri"), root.uri},
                                 {QStringLiteral("name"), root.name}});
    serverTransport->respond(requestId, {{QStringLiteral("roots"), roots}});
    emit rootsRequested(serverId, configuredRoots);
}

QString McpHostRuntime::invalidRequest(const QString& serverId,
                                       const QString& method,
                                       const QString& message)
{
    emit requestFailed(serverId, {}, method, QStringLiteral("invalid_request"),
                       message);
    return {};
}

void McpHostRuntime::handleResponse(const QString& serverId,
                                    const QString& requestId,
                                    const QString& method,
                                    const QJsonObject& result)
{
    const auto iterator = pending_.find(requestId);
    if (iterator == pending_.end()) return;
    const auto pending = iterator.value();
    pending_.erase(iterator);
    if (pending.serverId != serverId) return;

    if (pending.operation == Operation::Initialize)
    {
        McpInitializeResult initialization;
        QString errorMessage;
        if (!parseInitializeResult(result, initialization, errorMessage))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), errorMessage,
                          &pending);
            return;
        }
        serverRegistry_.setInitialization(serverId, initialization);
        serverRegistry_.clearError(serverId);
        if (!changeState(serverId, McpServerState::Ready)) return;
        if (auto* serverTransport = transport(serverId))
            serverTransport->notify(
                QStringLiteral("notifications/initialized"));
        publishSnapshot(serverId);
        emit serverInitialized(serverId, initialization.serverInfo);
        const auto connection = connections_.constFind(serverId);
        if (connection != connections_.constEnd() &&
            !connection->config.loggingLevel.isEmpty())
            setLoggingLevel(serverId, connection->config.loggingLevel);
        return;
    }
    if (pending.operation == Operation::Ping)
    {
        emit pingCompleted(serverId, requestId,
                           qMax<qint64>(0, notificationClock_.elapsed() -
                                               pending.startedAtMs));
        return;
    }
    if (pending.operation == Operation::ListTools)
    {
        const auto connection = connections_.constFind(serverId);
        if (connection == connections_.constEnd()) return;

        QList<agent::ToolDefinition> definitions;
        QString pageError;
        if (!parseToolPage(serverId, connection->config.toolAllowlist, result,
                           pending.tools, definitions, pageError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), pageError,
                          &pending);
            return;
        }

        const auto nextCursorValue = result.value(QStringLiteral("nextCursor"));
        if (!nextCursorValue.isUndefined() && !nextCursorValue.isNull())
        {
            const auto cursor = nextCursorValue.toString();
            if (!nextCursorValue.isString() || cursor.isEmpty() ||
                pending.cursors.contains(cursor))
            {
                handleFailure(
                    serverId, requestId, method,
                    QStringLiteral("invalid_response"),
                    QStringLiteral("tools/list returned an invalid or repeated "
                                   "nextCursor."),
                    &pending);
                return;
            }

            const auto nextRequestId = connection->transport->request(
                QStringLiteral("tools/list"),
                {{QStringLiteral("cursor"), cursor}},
                connection->config.requestTimeoutMs);
            if (nextRequestId.isEmpty())
            {
                recordCatalogFailure(
                    serverId, Operation::ListTools,
                    QStringLiteral("request_failed"),
                    QStringLiteral("Unable to request the next tools page."));
                publishSnapshot(serverId);
                refreshQueuedTools(serverId);
                return;
            }
            PendingRequest nextPending;
            nextPending.serverId = serverId;
            nextPending.operation = Operation::ListTools;
            nextPending.tools = std::move(definitions);
            nextPending.cursors = pending.cursors;
            nextPending.cursors.insert(cursor);
            pending_.insert(nextRequestId, std::move(nextPending));
            return;
        }

        QString registryError;
        if (!toolRegistry_.replaceServerTools(serverId, definitions,
                                              registryError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_tools"), registryError,
                          &pending);
            return;
        }
        serverRegistry_.replaceTools(serverId, definitions.size());
        recordCatalogSuccess(serverId, Operation::ListTools);
        publishSnapshot(serverId);
        emit toolsChanged(serverId, definitions);
        refreshQueuedTools(serverId);
        return;
    }

    if (pending.operation == Operation::ListResources)
    {
        const auto connection = connections_.constFind(serverId);
        if (connection == connections_.constEnd()) return;
        QList<McpResourceDefinition> definitions;
        QString pageError;
        if (!parseResourcePage(serverId, result, pending.resources, definitions,
                               pageError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), pageError,
                          &pending);
            return;
        }
        const auto nextCursorValue = result.value(QStringLiteral("nextCursor"));
        if (!nextCursorValue.isUndefined() && !nextCursorValue.isNull())
        {
            const auto cursor = nextCursorValue.toString();
            if (!nextCursorValue.isString() || cursor.isEmpty() ||
                pending.cursors.contains(cursor))
            {
                handleFailure(
                    serverId, requestId, method,
                    QStringLiteral("invalid_response"),
                    QStringLiteral("resources/list returned an invalid or "
                                   "repeated nextCursor."),
                    &pending);
                return;
            }
            const auto nextRequestId = connection->transport->request(
                QStringLiteral("resources/list"),
                {{QStringLiteral("cursor"), cursor}},
                connection->config.requestTimeoutMs);
            if (nextRequestId.isEmpty())
            {
                handleFailure(serverId, requestId, method,
                              QStringLiteral("request_failed"),
                              QStringLiteral("Unable to request the next "
                                             "resource page."),
                              &pending);
                return;
            }
            PendingRequest nextPending;
            nextPending.serverId = serverId;
            nextPending.operation = Operation::ListResources;
            nextPending.resources = std::move(definitions);
            nextPending.cursors = pending.cursors;
            nextPending.cursors.insert(cursor);
            pending_.insert(nextRequestId, std::move(nextPending));
            return;
        }
        QString registryError;
        if (!resourceRegistry_.replaceServerResources(serverId, definitions,
                                                      registryError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_resources"), registryError,
                          &pending);
            return;
        }
        serverRegistry_.replaceResources(
            serverId, definitions.size(),
            resourceRegistry_.templates(serverId).size());
        recordCatalogSuccess(serverId, Operation::ListResources);
        publishSnapshot(serverId);
        emit resourcesChanged(serverId, definitions);
        refreshQueuedResources(serverId);
        return;
    }

    if (pending.operation == Operation::ListResourceTemplates)
    {
        const auto connection = connections_.constFind(serverId);
        if (connection == connections_.constEnd()) return;
        QList<McpResourceTemplateDefinition> definitions;
        QString pageError;
        if (!parseResourceTemplatePage(serverId, result,
                                       pending.resourceTemplates, definitions,
                                       pageError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), pageError,
                          &pending);
            return;
        }
        const auto nextCursorValue = result.value(QStringLiteral("nextCursor"));
        if (!nextCursorValue.isUndefined() && !nextCursorValue.isNull())
        {
            const auto cursor = nextCursorValue.toString();
            if (!nextCursorValue.isString() || cursor.isEmpty() ||
                pending.cursors.contains(cursor))
            {
                handleFailure(
                    serverId, requestId, method,
                    QStringLiteral("invalid_response"),
                    QStringLiteral("resources/templates/list returned an "
                                   "invalid or repeated nextCursor."),
                    &pending);
                return;
            }
            const auto nextRequestId = connection->transport->request(
                QStringLiteral("resources/templates/list"),
                {{QStringLiteral("cursor"), cursor}},
                connection->config.requestTimeoutMs);
            if (nextRequestId.isEmpty())
            {
                handleFailure(serverId, requestId, method,
                              QStringLiteral("request_failed"),
                              QStringLiteral("Unable to request the next "
                                             "resource template page."),
                              &pending);
                return;
            }
            PendingRequest nextPending;
            nextPending.serverId = serverId;
            nextPending.operation = Operation::ListResourceTemplates;
            nextPending.resourceTemplates = std::move(definitions);
            nextPending.cursors = pending.cursors;
            nextPending.cursors.insert(cursor);
            pending_.insert(nextRequestId, std::move(nextPending));
            return;
        }
        QString registryError;
        if (!resourceRegistry_.replaceServerTemplates(serverId, definitions,
                                                      registryError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_resource_templates"),
                          registryError, &pending);
            return;
        }
        serverRegistry_.replaceResources(
            serverId, resourceRegistry_.resources(serverId).size(),
            definitions.size());
        recordCatalogSuccess(serverId, Operation::ListResourceTemplates);
        publishSnapshot(serverId);
        emit resourceTemplatesChanged(serverId, definitions);
        refreshQueuedResources(serverId);
        return;
    }

    if (pending.operation == Operation::ReadResource)
    {
        const auto connection = connections_.constFind(serverId);
        if (connection == connections_.constEnd()) return;
        McpResourceReadResult resourceResult;
        QString parseError;
        if (!parseResourceReadResult(requestId, serverId, pending.subject,
                                     result, connection->config.maxResultBytes,
                                     resourceResult, parseError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), parseError,
                          &pending);
            return;
        }
        emit resourceReadReady(resourceResult);
        return;
    }

    if (pending.operation == Operation::SubscribeResource ||
        pending.operation == Operation::UnsubscribeResource)
    {
        const auto subscribed =
            pending.operation == Operation::SubscribeResource;
        if (subscribed)
            resourceSubscriptions_[serverId].insert(pending.subject);
        else
            resourceSubscriptions_[serverId].remove(pending.subject);
        emit resourceSubscriptionChanged(serverId, pending.subject, subscribed);
        return;
    }

    if (pending.operation == Operation::ListPrompts)
    {
        const auto connection = connections_.constFind(serverId);
        if (connection == connections_.constEnd()) return;
        QList<McpPromptDefinition> definitions;
        QString pageError;
        if (!parsePromptPage(serverId, result, pending.prompts, definitions,
                             pageError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), pageError,
                          &pending);
            return;
        }
        const auto nextCursorValue = result.value(QStringLiteral("nextCursor"));
        if (!nextCursorValue.isUndefined() && !nextCursorValue.isNull())
        {
            const auto cursor = nextCursorValue.toString();
            if (!nextCursorValue.isString() || cursor.isEmpty() ||
                pending.cursors.contains(cursor))
            {
                handleFailure(
                    serverId, requestId, method,
                    QStringLiteral("invalid_response"),
                    QStringLiteral("prompts/list returned an invalid or "
                                   "repeated nextCursor."),
                    &pending);
                return;
            }
            const auto nextRequestId = connection->transport->request(
                QStringLiteral("prompts/list"),
                {{QStringLiteral("cursor"), cursor}},
                connection->config.requestTimeoutMs);
            if (nextRequestId.isEmpty())
            {
                handleFailure(serverId, requestId, method,
                              QStringLiteral("request_failed"),
                              QStringLiteral("Unable to request the next "
                                             "prompt page."),
                              &pending);
                return;
            }
            PendingRequest nextPending;
            nextPending.serverId = serverId;
            nextPending.operation = Operation::ListPrompts;
            nextPending.prompts = std::move(definitions);
            nextPending.cursors = pending.cursors;
            nextPending.cursors.insert(cursor);
            pending_.insert(nextRequestId, std::move(nextPending));
            return;
        }
        QString registryError;
        if (!promptRegistry_.replaceServerPrompts(serverId, definitions,
                                                  registryError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_prompts"), registryError,
                          &pending);
            return;
        }
        serverRegistry_.replacePrompts(serverId, definitions.size());
        recordCatalogSuccess(serverId, Operation::ListPrompts);
        publishSnapshot(serverId);
        emit promptsChanged(serverId, definitions);
        refreshQueuedPrompts(serverId);
        return;
    }

    if (pending.operation == Operation::GetPrompt)
    {
        const auto connection = connections_.constFind(serverId);
        if (connection == connections_.constEnd()) return;
        McpPromptResult promptResult;
        QString parseError;
        if (!parsePromptResult(requestId, serverId, pending.subject, result,
                               connection->config.maxResultBytes, promptResult,
                               parseError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), parseError,
                          &pending);
            return;
        }
        emit promptReady(promptResult);
        return;
    }

    if (pending.operation == Operation::Complete)
    {
        const auto connection = connections_.constFind(serverId);
        if (connection == connections_.constEnd()) return;
        McpCompletionResult completionResult;
        QString parseError;
        if (!parseCompletionResult(requestId, serverId, pending.referenceType,
                                   pending.subject, pending.toolName, result,
                                   connection->config.maxResultBytes,
                                   completionResult, parseError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_response"), parseError,
                          &pending);
            return;
        }
        emit completionReady(completionResult);
        return;
    }

    if (pending.operation == Operation::SetLoggingLevel)
    {
        const auto connection = connections_.find(serverId);
        if (connection == connections_.end()) return;
        connection->config.loggingLevel = pending.subject;
        serverRegistry_.setLoggingLevel(serverId, pending.subject);
        publishSnapshot(serverId);
        emit loggingLevelChanged(serverId, pending.subject);
        return;
    }

    agent::ToolResult toolResult;
    toolResult.requestId = requestId;
    toolResult.serverId = serverId;
    toolResult.toolName = pending.toolName;
    const auto structuredResult = structuredToolResult(result);
    const auto structuredFailure =
        structuredResult.isObject() &&
        structuredResult.toObject().value(QStringLiteral("ok")).isBool() &&
        !structuredResult.toObject().value(QStringLiteral("ok")).toBool();
    toolResult.isError =
        result.value(QStringLiteral("isError")).toBool() || structuredFailure;
    if (toolResult.isError)
    {
        toolResult.failureKind = agent::ToolFailureKind::Tool;
        toolResult.outcome = agent::ToolOutcome::ToolFailed;
        toolResult.sideEffectState = agent::ToolSideEffectState::KnownFailed;
        const auto error = structuredResult.isObject()
                               ? structuredResult.toObject()
                                     .value(QStringLiteral("error"))
                                     .toObject()
                               : QJsonObject{};
        toolResult.errorCode = error.value(QStringLiteral("code")).toString();
        toolResult.errorMessage =
            error.value(QStringLiteral("message")).toString().trimmed();
        if (toolResult.errorCode.isEmpty())
            toolResult.errorCode = QStringLiteral("mcp_tool_error");
        if (toolResult.errorMessage.isEmpty())
            toolResult.errorMessage = textToolResult(result);
        if (toolResult.errorMessage.isEmpty())
            toolResult.errorMessage =
                QStringLiteral("MCP tool reported an error.");
    }
    else
    {
        toolResult.sideEffectState = agent::ToolSideEffectState::Succeeded;
    }

    const auto qualifiedToolName =
        serverId + QLatin1Char('.') + pending.toolName;
    const auto* definition = toolRegistry_.find(qualifiedToolName);
    const auto hasOutputSchema =
        definition != nullptr &&
        (definition->hasOutputSchema || !definition->outputSchema.isEmpty());
    if (!toolResult.isError && hasOutputSchema)
    {
        QString outputError;
        if (toolRegistry_.validateOutput(qualifiedToolName, structuredResult,
                                         outputError))
        {
            toolResult.outputSchemaValidated = true;
        }
        else
        {
            toolResult.isError = true;
            toolResult.failureKind = agent::ToolFailureKind::Protocol;
            toolResult.outcome = agent::ToolOutcome::ProtocolFailed;
            toolResult.sideEffectState = agent::ToolSideEffectState::Uncertain;
            toolResult.errorCode = QStringLiteral("output_schema_mismatch");
            toolResult.errorMessage = outputError;
            emit serverError(serverId, toolResult.errorCode, outputError);
        }
    }

    const auto serialized =
        QJsonDocument(result).toJson(QJsonDocument::Compact);
    const auto connection = connections_.constFind(serverId);
    const auto maximum = connection == connections_.constEnd()
                             ? qsizetype{65'536}
                             : connection->config.maxResultBytes;
    toolResult.originalBytes = serialized.size();
    if (serialized.size() > maximum)
    {
        toolResult.truncated = true;
        toolResult.result = {
            {QStringLiteral("content"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                             {QStringLiteral("text"),
                              QString::fromUtf8(serialized.left(maximum))}}}},
            {QStringLiteral("_qtllmTruncated"), true},
            {QStringLiteral("_qtllmOriginalBytes"), serialized.size()}};
    }
    else
    {
        toolResult.result = result;
        toolResult.structuredContent = structuredResult;
        const auto content = result.value(QStringLiteral("content"));
        if (content.isArray())
        {
            toolResult.contentBlocks = content.toArray();
            static const QSet<QString> knownContentTypes{
                QStringLiteral("text"), QStringLiteral("image"),
                QStringLiteral("audio"), QStringLiteral("resource"),
                QStringLiteral("resource_link")};
            for (const auto& block : toolResult.contentBlocks)
            {
                const auto type =
                    block.toObject().value(QStringLiteral("type")).toString();
                if (!block.isObject() || type.isEmpty())
                    toolResult.unknownContentBlockTypes.append(
                        QStringLiteral("<invalid>"));
                else if (!knownContentTypes.contains(type))
                    toolResult.unknownContentBlockTypes.append(type);
            }
        }
    }
    emit toolResultReady(toolResult);
}

void McpHostRuntime::handleFailure(const QString& serverId,
                                   const QString& requestId,
                                   const QString& method, const QString& code,
                                   const QString& message,
                                   const PendingRequest* knownPending)
{
    PendingRequest pending;
    auto hasPending = false;
    if (knownPending != nullptr)
    {
        pending = *knownPending;
        hasPending = true;
    }
    else
    {
        const auto iterator = pending_.find(requestId);
        if (iterator != pending_.end())
        {
            pending = iterator.value();
            pending_.erase(iterator);
            hasPending = true;
        }
    }

    emit requestFailed(serverId, requestId, method, code, message);
    if (!hasPending) return;

    if (pending.operation == Operation::Initialize)
    {
        serverRegistry_.setError(serverId, code, message);
        revokeCapabilities(serverId);
        changeState(serverId, McpServerState::Failed);
        publishSnapshot(serverId);
        return;
    }
    if (pending.operation == Operation::ListTools)
    {
        recordCatalogFailure(serverId, Operation::ListTools, code, message);
        publishSnapshot(serverId);
        refreshQueuedTools(serverId);
        return;
    }
    if (pending.operation == Operation::ListResources ||
        pending.operation == Operation::ListResourceTemplates)
    {
        recordCatalogFailure(serverId, pending.operation, code, message);
        publishSnapshot(serverId);
        refreshQueuedResources(serverId);
        return;
    }
    if (pending.operation == Operation::ListPrompts)
    {
        recordCatalogFailure(serverId, Operation::ListPrompts, code, message);
        publishSnapshot(serverId);
        refreshQueuedPrompts(serverId);
        return;
    }
    if (pending.operation != Operation::CallTool) return;

    agent::ToolResult result;
    result.requestId = requestId;
    result.serverId = serverId;
    result.toolName = pending.toolName;
    result.isError = true;
    result.errorCode = code;
    result.errorMessage = message;
    result.failureKind = failureKind(code);
    result.outcome = failureOutcome(code);
    result.sideEffectState =
        requestId.isEmpty() ? agent::ToolSideEffectState::NotDispatched
                            : (result.outcome == agent::ToolOutcome::ToolFailed
                                   ? agent::ToolSideEffectState::KnownFailed
                                   : agent::ToolSideEffectState::Uncertain);
    emit toolResultReady(result);
}

bool McpHostRuntime::changeState(const QString& serverId, McpServerState state)
{
    const auto before = serverRegistry_.snapshot(serverId);
    QString errorMessage;
    if (!serverRegistry_.transition(serverId, state, errorMessage))
    {
        emit serverError(serverId, QStringLiteral("invalid_state_transition"),
                         errorMessage);
        return false;
    }
    if (!before || before->state != state)
        emit serverStateChanged(serverId, state);
    return true;
}

void McpHostRuntime::publishSnapshot(const QString& serverId)
{
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (snapshot) emit capabilitySnapshotChanged(*snapshot);
}

void McpHostRuntime::revokeCapabilities(const QString& serverId)
{
    const auto before = serverRegistry_.snapshot(serverId);
    queuedToolRefreshes_.remove(serverId);
    queuedResourceRefreshes_.remove(serverId);
    queuedPromptRefreshes_.remove(serverId);
    notificationWindows_.remove(serverId);
    catalogErrors_.remove(serverId);
    resourceSubscriptions_.remove(serverId);
    toolRegistry_.removeServer(serverId);
    resourceRegistry_.removeServer(serverId);
    promptRegistry_.removeServer(serverId);
    if (!serverRegistry_.revokeCapabilities(serverId)) return;
    if (before && before->toolCount > 0) emit toolsChanged(serverId, {});
    if (before && before->resourceCount > 0)
        emit resourcesChanged(serverId, {});
    if (before && before->resourceTemplateCount > 0)
        emit resourceTemplatesChanged(serverId, {});
    if (before && before->promptCount > 0) emit promptsChanged(serverId, {});
    publishSnapshot(serverId);
}

void McpHostRuntime::discardPendingRequests(const QString& serverId)
{
    for (auto iterator = pending_.begin(); iterator != pending_.end();)
    {
        if (iterator->serverId == serverId)
            iterator = pending_.erase(iterator);
        else
            ++iterator;
    }
}

bool McpHostRuntime::hasToolRefresh(const QString& serverId) const
{
    return hasPendingOperation(serverId, Operation::ListTools);
}

void McpHostRuntime::refreshQueuedTools(const QString& serverId)
{
    if (!queuedToolRefreshes_.remove(serverId)) return;
    listTools(serverId);
}

bool McpHostRuntime::hasPendingOperation(const QString& serverId,
                                         Operation operation) const
{
    for (auto iterator = pending_.constBegin(); iterator != pending_.constEnd();
         ++iterator)
        if (iterator->serverId == serverId && iterator->operation == operation)
            return true;
    return false;
}

void McpHostRuntime::refreshQueuedResources(const QString& serverId)
{
    if (!queuedResourceRefreshes_.contains(serverId) ||
        hasPendingOperation(serverId, Operation::ListResources) ||
        hasPendingOperation(serverId, Operation::ListResourceTemplates))
        return;
    queuedResourceRefreshes_.remove(serverId);
    listResources(serverId);
    listResourceTemplates(serverId);
}

void McpHostRuntime::refreshQueuedPrompts(const QString& serverId)
{
    if (!queuedPromptRefreshes_.contains(serverId) ||
        hasPendingOperation(serverId, Operation::ListPrompts))
        return;
    queuedPromptRefreshes_.remove(serverId);
    listPrompts(serverId);
}

void McpHostRuntime::recordCatalogFailure(const QString& serverId,
                                          Operation operation,
                                          const QString& code,
                                          const QString& message)
{
    catalogErrors_[serverId].insert(static_cast<int>(operation),
                                    {code, message});
    serverRegistry_.setError(serverId, code, message);
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (snapshot && snapshot->state == McpServerState::Ready)
        changeState(serverId, McpServerState::Degraded);
}

void McpHostRuntime::recordCatalogSuccess(const QString& serverId,
                                          Operation operation)
{
    auto serverErrors = catalogErrors_.find(serverId);
    if (serverErrors != catalogErrors_.end())
    {
        serverErrors->remove(static_cast<int>(operation));
        if (serverErrors->isEmpty())
        {
            catalogErrors_.erase(serverErrors);
            serverRegistry_.clearError(serverId);
        }
        else
        {
            const auto error = serverErrors->constBegin().value();
            serverRegistry_.setError(serverId, error.code, error.message);
        }
    }
    else
    {
        serverRegistry_.clearError(serverId);
    }
    const auto snapshot = serverRegistry_.snapshot(serverId);
    if (snapshot && snapshot->state == McpServerState::Degraded &&
        !catalogErrors_.contains(serverId))
        changeState(serverId, McpServerState::Ready);
}

bool McpHostRuntime::acceptNotification(const QString& serverId)
{
    const auto now = notificationClock_.elapsed();
    auto& window = notificationWindows_[serverId];
    if (now - window.startedAtMs >= notificationWindowMs)
    {
        window.startedAtMs = now;
        window.accepted = 0;
        window.reported = false;
    }
    if (window.accepted < maximumNotificationsPerWindow)
    {
        ++window.accepted;
        return true;
    }
    if (!window.reported)
    {
        window.reported = true;
        emit serverError(
            serverId, QStringLiteral("notification_rate_limited"),
            QStringLiteral("MCP notification rate exceeded 100 per second."));
    }
    return false;
}
}  // namespace qtllm::infrastructure::mcp
