#include "StdioMcpTransport.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

namespace qtllm::infrastructure::mcp
{
StdioMcpTransport::StdioMcpTransport(McpServerConfig config, QObject* parent)
    : McpTransport(parent), process_(std::move(config), this)
{
    connect(&process_, &McpServerProcess::started, this,
            &StdioMcpTransport::started);
    connect(&process_, &McpServerProcess::readyReadStandardOutput, this,
            &StdioMcpTransport::onReadyReadStandardOutput);
    connect(&process_, &McpServerProcess::readyReadStandardError, this,
            &StdioMcpTransport::onReadyReadStandardError);
    connect(&process_, &McpServerProcess::finished, this,
            &StdioMcpTransport::onProcessFinished);
    connect(&process_, &McpServerProcess::errorOccurred, this,
            &StdioMcpTransport::onProcessError);
}

StdioMcpTransport::~StdioMcpTransport()
{
    disconnect(&process_, nullptr, this, nullptr);
    stop();
}

const McpServerConfig& StdioMcpTransport::config() const
{
    return process_.config();
}

bool StdioMcpTransport::isRunning() const
{
    return process_.isRunning();
}

void StdioMcpTransport::start()
{
    if (!config().enabled)
    {
        emit transportError(QStringLiteral("server_disabled"),
                            QStringLiteral("MCP server is disabled."));
        return;
    }
    if (isRunning()) return;
    stopping_ = false;
    standardOutputBuffer_.clear();
    process_.start();
}

void StdioMcpTransport::stop()
{
    if (!isRunning())
    {
        cancelAll();
        return;
    }
    stopping_ = true;
    cancelAll();
    process_.stop();
}

QString StdioMcpTransport::request(const QString& method,
                                   const QJsonObject& params, int timeoutMs)
{
    if (method.trimmed().isEmpty())
    {
        emit requestFailed({}, method, QStringLiteral("invalid_request"),
                           QStringLiteral("MCP method must not be empty."));
        return {};
    }
    if (!isRunning())
    {
        emit requestFailed({}, method, QStringLiteral("not_running"),
                           QStringLiteral("MCP server is not running."));
        return {};
    }

    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return writeRequest(
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), requestId},
                    {QStringLiteral("method"), method},
                    {QStringLiteral("params"), params}},
        method, timeoutMs);
}

void StdioMcpTransport::cancel(const QString& requestId)
{
    if (requestId.isEmpty() || !pending_.contains(requestId)) return;
    failPending(requestId, QStringLiteral("cancelled"),
                QStringLiteral("MCP request was cancelled."));
    notify(QStringLiteral("notifications/cancelled"),
           {{QStringLiteral("requestId"), requestId},
            {QStringLiteral("reason"), QStringLiteral("cancelled")}});
}

void StdioMcpTransport::cancelAll()
{
    const auto requestIds = pending_.keys();
    for (const auto& requestId : requestIds)
        failPending(requestId, QStringLiteral("cancelled"),
                    QStringLiteral("MCP request was cancelled."));
}

bool StdioMcpTransport::notify(const QString& method, const QJsonObject& params)
{
    if (!isRunning() || method.trimmed().isEmpty()) return false;
    const auto object =
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), method},
                    {QStringLiteral("params"), params}};
    return writeMessage(object);
}

bool StdioMcpTransport::respond(const QJsonValue& requestId,
                                const QJsonObject& result)
{
    if (!isRunning() || (!requestId.isString() && !requestId.isDouble()))
        return false;
    return writeMessage({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), requestId},
                         {QStringLiteral("result"), result}});
}

bool StdioMcpTransport::respondError(const QJsonValue& requestId, int code,
                                     const QString& message)
{
    if (!isRunning() || (!requestId.isString() && !requestId.isDouble()))
        return false;
    return writeMessage({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), requestId},
                         {QStringLiteral("error"),
                          QJsonObject{{QStringLiteral("code"), code},
                                      {QStringLiteral("message"), message}}}});
}

QString StdioMcpTransport::writeRequest(const QJsonObject& object,
                                        const QString& method, int timeoutMs)
{
    const auto requestId = object.value(QStringLiteral("id")).toString();
    const auto data =
        QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (process_.write(data) != data.size())
    {
        emit requestFailed(requestId, method, QStringLiteral("write_failed"),
                           QStringLiteral("Unable to write MCP request."));
        return {};
    }

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    const auto boundedTimeout = qBound(1'000, timeoutMs, 300'000);
    connect(timer, &QTimer::timeout, this,
            [this, requestId]
            {
                failPending(requestId, QStringLiteral("timeout"),
                            QStringLiteral("MCP request timed out."));
            });
    pending_.insert(requestId, {method, timer});
    timer->start(boundedTimeout);
    return requestId;
}

bool StdioMcpTransport::writeMessage(const QJsonObject& object)
{
    const auto data =
        QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    return process_.write(data) == data.size();
}

void StdioMcpTransport::onReadyReadStandardOutput()
{
    standardOutputBuffer_ += process_.readStandardOutput();
    while (true)
    {
        const auto newline = standardOutputBuffer_.indexOf('\n');
        if (newline < 0) break;
        if (newline > 1'048'576)
        {
            emit transportError(
                QStringLiteral("message_too_large"),
                QStringLiteral("MCP stdout message exceeds 1 MiB."));
            standardOutputBuffer_.remove(0, newline + 1);
            continue;
        }
        const auto line = standardOutputBuffer_.left(newline).trimmed();
        standardOutputBuffer_.remove(0, newline + 1);
        if (!line.isEmpty()) processLine(line);
    }
    if (standardOutputBuffer_.size() > 1'048'576)
    {
        emit transportError(
            QStringLiteral("message_too_large"),
            QStringLiteral("MCP stdout message exceeds 1 MiB."));
        standardOutputBuffer_.clear();
    }
}

void StdioMcpTransport::onReadyReadStandardError()
{
    const auto text = QString::fromUtf8(process_.readStandardError());
    if (!text.isEmpty()) emit diagnosticReceived(text);
}

void StdioMcpTransport::onProcessFinished(int exitCode,
                                          QProcess::ExitStatus exitStatus)
{
    cancelAll();
    standardOutputBuffer_.clear();
    emit stopped();
    if (!stopping_)
        emit transportError(
            QStringLiteral("process_exited"),
            QStringLiteral("MCP server exited with code %1 (%2).")
                .arg(exitCode)
                .arg(exitStatus == QProcess::CrashExit
                         ? QStringLiteral("crashed")
                         : QStringLiteral("normal")));
}

void StdioMcpTransport::onProcessError(QProcess::ProcessError error,
                                       const QString& message)
{
    if (stopping_) return;
    emit transportError(QStringLiteral("process_error"),
                        QStringLiteral("MCP process error %1: %2")
                            .arg(static_cast<int>(error))
                            .arg(message));
}

void StdioMcpTransport::processLine(const QByteArray& line)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        emit transportError(
            QStringLiteral("invalid_message"),
            QStringLiteral("MCP stdout is not a JSON object: %1")
                .arg(parseError.errorString()));
        return;
    }

    const auto object = document.object();
    if (object.value(QStringLiteral("jsonrpc")).toString() !=
        QStringLiteral("2.0"))
    {
        emit transportError(
            QStringLiteral("invalid_message"),
            QStringLiteral("MCP message must use JSON-RPC 2.0."));
        return;
    }

    const auto idValue = object.value(QStringLiteral("id"));
    if (idValue.isUndefined() || idValue.isNull())
    {
        const auto method = object.value(QStringLiteral("method"));
        const auto params = object.value(QStringLiteral("params"));
        if (!method.isString() ||
            (!params.isUndefined() && !params.isNull() && !params.isObject()))
        {
            emit transportError(QStringLiteral("invalid_message"),
                                QStringLiteral("Invalid MCP notification."));
            return;
        }
        emit notificationReceived(method.toString(), params.toObject());
        return;
    }
    if (!idValue.isString() && !idValue.isDouble())
    {
        emit transportError(QStringLiteral("invalid_message"),
                            QStringLiteral("MCP response id is invalid."));
        return;
    }

    const auto method = object.value(QStringLiteral("method"));
    if (!method.isUndefined())
    {
        const auto params = object.value(QStringLiteral("params"));
        if (!method.isString() || method.toString().trimmed().isEmpty())
        {
            emit transportError(QStringLiteral("invalid_message"),
                                QStringLiteral("Invalid MCP request method."));
            return;
        }
        if (!params.isUndefined() && !params.isNull() && !params.isObject())
        {
            respondError(idValue, -32602,
                         QStringLiteral("Request params must be an object."));
            return;
        }
        emit requestReceived(idValue, method.toString(), params.toObject());
        return;
    }

    const auto requestId = idValue.isString()
                               ? idValue.toString()
                               : QString::number(idValue.toInteger());
    if (!pending_.contains(requestId)) return;
    const auto pending = pending_.value(requestId);
    if (object.value(QStringLiteral("error")).isObject())
    {
        const auto error = object.value(QStringLiteral("error")).toObject();
        failPending(requestId, QStringLiteral("remote_error"),
                    error.value(QStringLiteral("message")).toString());
        return;
    }
    const auto result = object.value(QStringLiteral("result"));
    if (!result.isObject())
    {
        failPending(requestId, QStringLiteral("invalid_response"),
                    QStringLiteral("MCP result must be an object."));
        return;
    }
    if (pending.timer != nullptr)
    {
        pending.timer->stop();
        pending.timer->deleteLater();
    }
    pending_.remove(requestId);
    emit responseReceived(requestId, pending.method, result.toObject());
}

void StdioMcpTransport::failPending(const QString& requestId,
                                    const QString& code, const QString& message)
{
    const auto iterator = pending_.find(requestId);
    if (iterator == pending_.end()) return;
    const auto method = iterator->method;
    if (iterator->timer != nullptr)
    {
        iterator->timer->stop();
        iterator->timer->deleteLater();
    }
    pending_.erase(iterator);
    emit requestFailed(requestId, method, code, message);
}
}  // namespace qtllm::infrastructure::mcp
