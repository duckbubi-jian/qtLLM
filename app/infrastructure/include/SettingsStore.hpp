#pragma once

#include "McpServerProcess.hpp"

#include <QList>
#include <QString>
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
    [[nodiscard]] bool isModelFileVerified(const QString& modelPath,
                                           qint64 expectedSize,
                                           const QString& expectedSha256) const;
    bool setModelFileVerified(const QString& modelPath, qint64 expectedSize,
                              const QString& expectedSha256) const;
    [[nodiscard]] QList<mcp::McpServerConfig> mcpServerConfigs() const;
    bool setMcpServerConfigs(
        const QList<mcp::McpServerConfig>& configurations) const;

   private:
    [[nodiscard]] static QString verificationKey(const QString& modelPath);
    [[nodiscard]] QString mcpConfigFilePath() const;

    QString filePath_;
};
}  // namespace qtllm::infrastructure
