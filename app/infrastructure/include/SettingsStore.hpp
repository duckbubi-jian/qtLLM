#pragma once

#include "McpServerProcess.hpp"

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace qtllm::infrastructure
{
class SettingsStore final
{
   public:
    explicit SettingsStore(QString filePath = {});

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QString lastModelPath() const;
    bool setLastModelPath(const QString& modelPath) const;
    [[nodiscard]] QString workspacePath() const;
    bool setWorkspacePath(const QString& workspacePath) const;
    [[nodiscard]] bool agentModeEnabled() const;
    bool setAgentModeEnabled(bool enabled) const;
    [[nodiscard]] bool isModelFileVerified(const QString& modelPath,
                                           qint64 expectedSize,
                                           const QString& expectedSha256) const;
    bool setModelFileVerified(const QString& modelPath, qint64 expectedSize,
                              const QString& expectedSha256) const;
    [[nodiscard]] QList<mcp::McpServerConfig> mcpServerConfigs() const;
    bool setMcpServerConfigs(
        const QList<mcp::McpServerConfig>& configurations) const;
    [[nodiscard]] QStringList alwaysAllowedMcpTools() const;
    bool setAlwaysAllowedMcpTools(const QStringList& qualifiedToolNames) const;

   private:
    [[nodiscard]] static QString verificationKey(const QString& modelPath);
    [[nodiscard]] QString mcpConfigFilePath() const;

    QString filePath_;
};
}  // namespace qtllm::infrastructure
