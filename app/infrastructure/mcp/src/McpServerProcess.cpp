#include "McpServerProcess.hpp"

#include <QJsonArray>
#include <QProcessEnvironment>

namespace qtllm::infrastructure::mcp
{
QJsonObject serializeServerConfig(const McpServerConfig& config)
{
    QJsonObject environment;
    for (auto item = config.environment.constBegin();
         item != config.environment.constEnd(); ++item)
        environment.insert(item.key(), item.value());
    return {{QStringLiteral("serverId"), config.serverId},
            {QStringLiteral("program"), config.program},
            {QStringLiteral("arguments"),
             QJsonArray::fromStringList(config.arguments)},
            {QStringLiteral("environment"), environment},
            {QStringLiteral("workingDirectory"), config.workingDirectory},
            {QStringLiteral("enabled"), config.enabled},
            {QStringLiteral("toolAllowlist"),
             QJsonArray::fromStringList(config.toolAllowlist)},
            {QStringLiteral("initializeTimeoutMs"), config.initializeTimeoutMs},
            {QStringLiteral("requestTimeoutMs"), config.requestTimeoutMs},
            {QStringLiteral("maxResultBytes"), config.maxResultBytes}};
}

bool parseServerConfig(const QJsonObject& object, McpServerConfig& config,
                       QString& errorMessage)
{
    const auto serverId = object.value(QStringLiteral("serverId"));
    const auto program = object.value(QStringLiteral("program"));
    const auto arguments = object.value(QStringLiteral("arguments"));
    const auto environment = object.value(QStringLiteral("environment"));
    const auto allowlist = object.value(QStringLiteral("toolAllowlist"));
    if (!serverId.isString() || serverId.toString().trimmed().isEmpty() ||
        !program.isString() || program.toString().trimmed().isEmpty() ||
        (!arguments.isUndefined() && !arguments.isArray()) ||
        (!environment.isUndefined() && !environment.isObject()) ||
        (!allowlist.isUndefined() && !allowlist.isArray()))
    {
        errorMessage = QStringLiteral("Invalid MCP server configuration.");
        return false;
    }

    McpServerConfig parsed;
    parsed.serverId = serverId.toString().trimmed();
    parsed.program = program.toString();
    parsed.workingDirectory =
        object.value(QStringLiteral("workingDirectory")).toString();
    parsed.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    auto readStringArray =
        [&errorMessage](const QJsonValue& value, QStringList& target)
    {
        for (const auto& item : value.toArray())
        {
            if (!item.isString())
            {
                errorMessage = QStringLiteral(
                    "MCP arguments and toolAllowlist must contain strings.");
                return false;
            }
            target.append(item.toString());
        }
        return true;
    };
    if (!readStringArray(arguments, parsed.arguments) ||
        !readStringArray(allowlist, parsed.toolAllowlist))
        return false;
    const auto environmentObject = environment.toObject();
    for (auto item = environmentObject.constBegin();
         item != environmentObject.constEnd(); ++item)
    {
        if (!item->isString())
        {
            errorMessage =
                QStringLiteral("MCP environment values must be strings.");
            return false;
        }
        parsed.environment.insert(item.key(), item->toString());
    }
    const auto initializeTimeout =
        object.value(QStringLiteral("initializeTimeoutMs"));
    const auto requestTimeout =
        object.value(QStringLiteral("requestTimeoutMs"));
    const auto maxResult = object.value(QStringLiteral("maxResultBytes"));
    if (initializeTimeout.isDouble())
        parsed.initializeTimeoutMs = initializeTimeout.toInteger();
    if (requestTimeout.isDouble())
        parsed.requestTimeoutMs = requestTimeout.toInteger();
    if (maxResult.isDouble()) parsed.maxResultBytes = maxResult.toInteger();
    if (parsed.initializeTimeoutMs < 1'000 ||
        parsed.initializeTimeoutMs > 300'000 ||
        parsed.requestTimeoutMs < 1'000 || parsed.requestTimeoutMs > 300'000 ||
        parsed.maxResultBytes < 1'024 || parsed.maxResultBytes > 1'048'576)
    {
        errorMessage =
            QStringLiteral("MCP configuration limit is out of range.");
        return false;
    }
    config = std::move(parsed);
    return true;
}

McpServerProcess::McpServerProcess(McpServerConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config))
{
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::started, this, &McpServerProcess::started);
    connect(&process_, &QProcess::readyReadStandardOutput, this,
            &McpServerProcess::readyReadStandardOutput);
    connect(&process_, &QProcess::readyReadStandardError, this,
            &McpServerProcess::readyReadStandardError);
    connect(&process_, &QProcess::finished, this, &McpServerProcess::finished);
    connect(&process_, &QProcess::errorOccurred, this,
            &McpServerProcess::onProcessError);
}

const McpServerConfig& McpServerProcess::config() const
{
    return config_;
}

QProcess::ProcessState McpServerProcess::state() const
{
    return process_.state();
}

bool McpServerProcess::isRunning() const
{
    return process_.state() != QProcess::NotRunning;
}

void McpServerProcess::start()
{
    if (isRunning()) return;
    if (config_.program.isEmpty())
    {
        emit errorOccurred(QProcess::FailedToStart,
                           QStringLiteral("MCP server program is empty."));
        return;
    }

    auto environment = QProcessEnvironment::systemEnvironment();
    for (auto item = config_.environment.constBegin();
         item != config_.environment.constEnd(); ++item)
        environment.insert(item.key(), item.value());
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(config_.workingDirectory);
    process_.setProgram(config_.program);
    process_.setArguments(config_.arguments);
    stopping_ = false;
    process_.start();
}

void McpServerProcess::stop()
{
    if (!isRunning()) return;
    stopping_ = true;
    process_.closeWriteChannel();
    if (!process_.waitForFinished(1000)) process_.terminate();
    if (!process_.waitForFinished(1000)) process_.kill();
}

void McpServerProcess::cancelPendingShutdown()
{
    stopping_ = false;
}

qint64 McpServerProcess::write(const QByteArray& data)
{
    return process_.write(data);
}

QByteArray McpServerProcess::readStandardOutput()
{
    return process_.readAllStandardOutput();
}

QByteArray McpServerProcess::readStandardError()
{
    return process_.readAllStandardError();
}

void McpServerProcess::onProcessError(QProcess::ProcessError error)
{
    if (stopping_ && error == QProcess::Crashed) return;
    emit errorOccurred(error, process_.errorString());
}
}  // namespace qtllm::infrastructure::mcp
