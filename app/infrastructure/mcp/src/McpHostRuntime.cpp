#include "McpHostRuntime.hpp"

#include "McpProtocol.hpp"
#include "StdioMcpTransport.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

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
    qRegisterMetaType<McpServerState>();
    qRegisterMetaType<McpServerSnapshot>();
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

    const auto serverId = config.serverId;
    auto* transportPointer = transport.get();
    connections_.insert(serverId,
                        {std::move(config), std::move(transport), false});
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

QString McpHostRuntime::agentInstructions() const
{
    QStringList instructions;
    for (const auto& snapshot : serverRegistry_.snapshots())
    {
        if ((snapshot.state != McpServerState::Ready &&
             snapshot.state != McpServerState::Degraded) ||
            snapshot.instructions.trimmed().isEmpty())
            continue;
        instructions.append(QStringLiteral("%1: %2").arg(
            snapshot.serverId,
            snapshot.instructions.trimmed().left(maximumServerInstructions)));
    }
    return instructions.join(QLatin1Char('\n'))
        .left(maximumCombinedInstructions);
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

    revokeCapabilities(serverId);
    serverRegistry_.clearError(serverId);
    iterator->stopRequested = false;
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
         {QStringLiteral("capabilities"), QJsonObject{}},
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
                discardPendingRequests(serverId);
                revokeCapabilities(serverId);
                changeState(serverId, requested ? McpServerState::Stopped
                                                : McpServerState::Failed);
                iterator->stopRequested = false;
                emit serverStopped(serverId);
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
    connect(transportPointer, &McpTransport::diagnosticReceived, this,
            [this, serverId](const QString& text)
            { emit diagnosticReceived(serverId, text); });
    connect(transportPointer, &McpTransport::transportError, this,
            [this, serverId](const QString& code, const QString& message)
            {
                if (!connections_.contains(serverId)) return;
                discardPendingRequests(serverId);
                serverRegistry_.setError(serverId, code, message);
                revokeCapabilities(serverId);
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
    emit notificationReceived(serverId, method, params);
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
                serverRegistry_.setError(
                    serverId, QStringLiteral("request_failed"),
                    QStringLiteral("Unable to request the next tools page."));
                const auto snapshot = serverRegistry_.snapshot(serverId);
                if (snapshot && snapshot->state == McpServerState::Ready)
                    changeState(serverId, McpServerState::Degraded);
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
        serverRegistry_.clearError(serverId);
        const auto snapshot = serverRegistry_.snapshot(serverId);
        if (snapshot && snapshot->state == McpServerState::Degraded)
            changeState(serverId, McpServerState::Ready);
        publishSnapshot(serverId);
        emit toolsChanged(serverId, definitions);
        refreshQueuedTools(serverId);
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

        QString outputError;
        if (!toolResult.isError &&
            !toolRegistry_.validateOutput(
                serverId + QLatin1Char('.') + pending.toolName,
                structuredResult, outputError))
            emit serverError(serverId, QStringLiteral("output_schema_mismatch"),
                             outputError);
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
        serverRegistry_.setError(serverId, code, message);
        const auto snapshot = serverRegistry_.snapshot(serverId);
        if (snapshot && snapshot->state == McpServerState::Ready)
            changeState(serverId, McpServerState::Degraded);
        publishSnapshot(serverId);
        refreshQueuedTools(serverId);
        return;
    }

    agent::ToolResult result;
    result.requestId = requestId;
    result.serverId = serverId;
    result.toolName = pending.toolName;
    result.isError = true;
    result.errorCode = code;
    result.errorMessage = message;
    result.failureKind = failureKind(code);
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
    notificationWindows_.remove(serverId);
    toolRegistry_.removeServer(serverId);
    if (!serverRegistry_.revokeCapabilities(serverId)) return;
    if (before && before->toolCount > 0) emit toolsChanged(serverId, {});
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
    for (auto iterator = pending_.constBegin(); iterator != pending_.constEnd();
         ++iterator)
        if (iterator->serverId == serverId &&
            iterator->operation == Operation::ListTools)
            return true;
    return false;
}

void McpHostRuntime::refreshQueuedTools(const QString& serverId)
{
    if (!queuedToolRefreshes_.remove(serverId)) return;
    listTools(serverId);
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
