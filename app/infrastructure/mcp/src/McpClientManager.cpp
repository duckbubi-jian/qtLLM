#include "McpClientManager.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace qtllm::infrastructure::mcp
{
namespace
{
constexpr auto clientName = "qtLLM";
constexpr auto clientVersion = "0.2.0";
constexpr qsizetype maximumServerInstructions = 4'096;
constexpr qsizetype maximumCombinedInstructions = 8'192;

QJsonObject structuredToolResult(const QJsonObject& result)
{
    const auto structured = result.value(QStringLiteral("structuredContent"));
    return structured.isObject() ? structured.toObject() : QJsonObject{};
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
}  // namespace

McpClientManager::McpClientManager(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<agent::ToolDefinition>();
    qRegisterMetaType<agent::ToolResult>();
}

McpClientManager::~McpClientManager()
{
    for (auto& server : servers_)
    {
        disconnect(server.transport.get(), nullptr, this, nullptr);
        server.transport->stop();
    }
    pending_.clear();
}

bool McpClientManager::addServer(McpServerConfig config, QString& errorMessage)
{
    config.serverId = config.serverId.trimmed();
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
    if (servers_.contains(config.serverId))
    {
        errorMessage = QStringLiteral("MCP serverId is already registered: %1")
                           .arg(config.serverId);
        return false;
    }

    auto transport = QSharedPointer<StdioMcpTransport>::create(config);
    auto* transportPointer = transport.get();
    servers_.insert(config.serverId, {transport, {}, {}, false});
    connectTransport(config.serverId, transportPointer);
    return true;
}

bool McpClientManager::removeServer(const QString& serverId,
                                    QString& errorMessage)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end())
    {
        errorMessage = QStringLiteral("Unknown MCP server: %1").arg(serverId);
        return false;
    }
    iterator->transport->stop();
    registry_.removeServer(serverId);
    for (auto pending = pending_.begin(); pending != pending_.end();)
    {
        if (pending->serverId == serverId)
            pending = pending_.erase(pending);
        else
            ++pending;
    }
    servers_.erase(iterator);
    return true;
}

QStringList McpClientManager::serverIds() const
{
    return servers_.keys();
}

StdioMcpTransport* McpClientManager::transport(const QString& serverId) const
{
    const auto iterator = servers_.constFind(serverId);
    return iterator == servers_.constEnd() ? nullptr
                                           : iterator->transport.get();
}

QList<agent::ToolDefinition> McpClientManager::tools() const
{
    return registry_.tools();
}

QString McpClientManager::agentInstructions() const
{
    auto serverIds = servers_.keys();
    serverIds.sort(Qt::CaseSensitive);
    QStringList instructions;
    for (const auto& serverId : serverIds)
    {
        const auto server = servers_.constFind(serverId);
        if (server == servers_.constEnd() || !server->initialized ||
            server->initialization.instructions.trimmed().isEmpty())
            continue;
        instructions.append(QStringLiteral("%1: %2").arg(
            serverId, server->initialization.instructions.trimmed().left(
                          maximumServerInstructions)));
    }
    return instructions.join(QLatin1Char('\n'))
        .left(maximumCombinedInstructions);
}

const ToolRegistry& McpClientManager::registry() const
{
    return registry_;
}

void McpClientManager::startServer(const QString& serverId)
{
    if (auto* serverTransport = transport(serverId))
        serverTransport->start();
    else
        emit serverError(serverId, QStringLiteral("unknown_server"),
                         QStringLiteral("Unknown MCP server."));
}

void McpClientManager::stopServer(const QString& serverId)
{
    if (auto* serverTransport = transport(serverId))
        serverTransport->stop();
    else
        emit serverError(serverId, QStringLiteral("unknown_server"),
                         QStringLiteral("Unknown MCP server."));
}

QString McpClientManager::initialize(const QString& serverId)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end())
        return invalidRequest(serverId, QStringLiteral("initialize"),
                              QStringLiteral("Unknown MCP server."));
    if (iterator->initialized) return {};
    const auto requestId = iterator->transport->request(
        QStringLiteral("initialize"),
        {{QStringLiteral("protocolVersion"), latestSupportedProtocolVersion()},
         {QStringLiteral("capabilities"), QJsonObject{}},
         {QStringLiteral("clientInfo"),
          QJsonObject{{QStringLiteral("name"), QString::fromLatin1(clientName)},
                      {QStringLiteral("version"),
                       QString::fromLatin1(clientVersion)}}}},
        iterator->transport->config().initializeTimeoutMs);
    if (!requestId.isEmpty())
        pending_.insert(requestId, {serverId, Operation::Initialize, {}});
    return requestId;
}

QString McpClientManager::listTools(const QString& serverId)
{
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end())
        return invalidRequest(serverId, QStringLiteral("tools/list"),
                              QStringLiteral("Unknown MCP server."));
    if (!iterator->initialized)
        return invalidRequest(serverId, QStringLiteral("tools/list"),
                              QStringLiteral("MCP server is not initialized."));
    if (!iterator->initialization.capabilities.tools)
        return invalidRequest(
            serverId, QStringLiteral("tools/list"),
            QStringLiteral("MCP server did not declare the Tools capability."));
    const auto requestId = iterator->transport->request(
        QStringLiteral("tools/list"), {},
        iterator->transport->config().requestTimeoutMs);
    if (!requestId.isEmpty())
        pending_.insert(requestId, {serverId, Operation::ListTools, {}});
    return requestId;
}

QString McpClientManager::callTool(const QString& qualifiedToolName,
                                   const QJsonObject& arguments)
{
    const auto separator = qualifiedToolName.indexOf(QLatin1Char('.'));
    if (separator <= 0 || separator == qualifiedToolName.size() - 1)
        return invalidRequest(
            {}, QStringLiteral("tools/call"),
            QStringLiteral("Tool name must be serverId.name."));
    const auto serverId = qualifiedToolName.left(separator);
    const auto toolName = qualifiedToolName.mid(separator + 1);
    const auto iterator = servers_.find(serverId);
    if (iterator == servers_.end())
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              QStringLiteral("Unknown MCP server."));
    if (!iterator->initialized)
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              QStringLiteral("MCP server is not initialized."));
    if (!iterator->initialization.capabilities.tools)
        return invalidRequest(
            serverId, QStringLiteral("tools/call"),
            QStringLiteral("MCP server did not declare the Tools capability."));

    if (registry_.find(qualifiedToolName) == nullptr)
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              QStringLiteral("Tool is not registered: %1")
                                  .arg(qualifiedToolName));
    QString validationError;
    if (!registry_.validateArguments(qualifiedToolName, arguments,
                                     validationError))
        return invalidRequest(serverId, QStringLiteral("tools/call"),
                              validationError);

    const auto requestId = iterator->transport->request(
        QStringLiteral("tools/call"),
        {{QStringLiteral("name"), toolName},
         {QStringLiteral("arguments"), arguments}},
        iterator->transport->config().requestTimeoutMs);
    if (!requestId.isEmpty())
        pending_.insert(requestId, {serverId, Operation::CallTool, toolName});
    return requestId;
}

void McpClientManager::cancel(const QString& requestId)
{
    const auto iterator = pending_.find(requestId);
    if (iterator == pending_.end()) return;
    if (auto* serverTransport = transport(iterator->serverId))
        serverTransport->cancel(requestId);
}

void McpClientManager::cancelAll()
{
    for (auto& server : servers_)
        server.transport->cancelAll();
}

void McpClientManager::connectTransport(const QString& serverId,
                                        StdioMcpTransport* transportPointer)
{
    connect(transportPointer, &StdioMcpTransport::started, this,
            [this, serverId] { emit serverStarted(serverId); });
    connect(transportPointer, &StdioMcpTransport::stopped, this,
            [this, serverId]
            {
                if (auto iterator = servers_.find(serverId);
                    iterator != servers_.end())
                {
                    iterator->initialized = false;
                    iterator->initialization = {};
                }
                emit serverStopped(serverId);
            });
    connect(transportPointer, &StdioMcpTransport::responseReceived, this,
            [this, serverId](const QString& requestId, const QString& method,
                             const QJsonObject& result)
            { handleResponse(serverId, requestId, method, result); });
    connect(transportPointer, &StdioMcpTransport::requestFailed, this,
            [this, serverId](const QString& requestId, const QString& method,
                             const QString& code, const QString& message)
            { handleFailure(serverId, requestId, method, code, message); });
    connect(transportPointer, &StdioMcpTransport::diagnosticReceived, this,
            [this, serverId](const QString& text)
            { emit diagnosticReceived(serverId, text); });
    connect(transportPointer, &StdioMcpTransport::transportError, this,
            [this, serverId](const QString& code, const QString& message)
            { emit serverError(serverId, code, message); });
}

QString McpClientManager::invalidRequest(const QString& serverId,
                                         const QString& method,
                                         const QString& message)
{
    emit requestFailed(serverId, {}, method, QStringLiteral("invalid_request"),
                       message);
    return {};
}

void McpClientManager::handleResponse(const QString& serverId,
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
                          QStringLiteral("invalid_response"), errorMessage);
            return;
        }
        if (auto server = servers_.find(serverId); server != servers_.end())
        {
            server->initialized = true;
            server->initialization = initialization;
        }
        if (auto* serverTransport = transport(serverId))
            serverTransport->notify(
                QStringLiteral("notifications/initialized"));
        emit serverInitialized(serverId, initialization.serverInfo);
        return;
    }
    if (pending.operation == Operation::ListTools)
    {
        QList<agent::ToolDefinition> definitions;
        const auto toolsValue = result.value(QStringLiteral("tools"));
        const auto serverIterator = servers_.find(serverId);
        if (!toolsValue.isArray() || serverIterator == servers_.end())
        {
            handleFailure(
                serverId, requestId, method, QStringLiteral("invalid_response"),
                QStringLiteral("tools/list result has no tools array."));
            return;
        }
        const auto allowlist =
            serverIterator->transport->config().toolAllowlist;
        for (const auto& value : toolsValue.toArray())
        {
            if (!value.isObject()) continue;
            const auto object = value.toObject();
            const auto name =
                object.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty() ||
                !object.value(QStringLiteral("inputSchema")).isObject() ||
                (!allowlist.isEmpty() && !allowlist.contains(name)))
                continue;
            definitions.append(
                {serverId + QLatin1Char('.') + name, serverId, name,
                 object.value(QStringLiteral("description")).toString(),
                 object.value(QStringLiteral("inputSchema")).toObject()});
        }
        serverIterator->tools = definitions;
        QString registryError;
        if (!registry_.replaceServerTools(serverId, definitions, registryError))
        {
            handleFailure(serverId, requestId, method,
                          QStringLiteral("invalid_tools"), registryError);
            return;
        }
        emit toolsChanged(serverId, definitions);
        return;
    }

    agent::ToolResult toolResult;
    toolResult.requestId = requestId;
    toolResult.serverId = serverId;
    toolResult.toolName = pending.toolName;
    const auto structuredResult = structuredToolResult(result);
    const auto structuredFailure =
        structuredResult.value(QStringLiteral("ok")).isBool() &&
        !structuredResult.value(QStringLiteral("ok")).toBool();
    toolResult.isError =
        result.value(QStringLiteral("isError")).toBool() || structuredFailure;
    if (toolResult.isError)
    {
        const auto error =
            structuredResult.value(QStringLiteral("error")).toObject();
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
    const auto server = servers_.constFind(serverId);
    const auto maximum = server == servers_.constEnd()
                             ? qsizetype{65'536}
                             : server->transport->config().maxResultBytes;
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
    }
    emit toolResultReady(toolResult);
}

void McpClientManager::handleFailure(const QString& serverId,
                                     const QString& requestId,
                                     const QString& method, const QString& code,
                                     const QString& message)
{
    const auto iterator = pending_.find(requestId);
    PendingRequest pending;
    if (iterator != pending_.end())
    {
        pending = iterator.value();
        pending_.erase(iterator);
    }
    emit requestFailed(serverId, requestId, method, code, message);
    if (pending.operation == Operation::CallTool)
    {
        agent::ToolResult result;
        result.requestId = requestId;
        result.serverId = serverId;
        result.toolName = pending.toolName;
        result.isError = true;
        result.errorCode = code;
        result.errorMessage = message;
        emit toolResultReady(result);
    }
}
}  // namespace qtllm::infrastructure::mcp
