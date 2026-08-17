#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace qtllm::infrastructure::mcp
{
struct McpServerCapabilities
{
    bool tools = false;
    bool toolsListChanged = false;
    bool resources = false;
    bool resourcesSubscribe = false;
    bool resourcesListChanged = false;
    bool prompts = false;
    bool promptsListChanged = false;
    bool logging = false;
    bool completions = false;
    QJsonObject raw;
};

struct McpInitializeResult
{
    QString protocolVersion;
    McpServerCapabilities capabilities;
    QJsonObject serverInfo;
    QString instructions;
};

[[nodiscard]] QString latestSupportedProtocolVersion();
[[nodiscard]] QStringList supportedProtocolVersions();
[[nodiscard]] bool isSupportedProtocolVersion(const QString& version);
[[nodiscard]] QStringList supportedLoggingLevels();
[[nodiscard]] bool isSupportedLoggingLevel(const QString& level);
[[nodiscard]] bool parseInitializeResult(const QJsonObject& object,
                                         McpInitializeResult& result,
                                         QString& errorMessage);
}  // namespace qtllm::infrastructure::mcp
