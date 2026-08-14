#pragma once

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

   private:
    [[nodiscard]] static QString verificationKey(const QString& modelPath);

    QString filePath_;
};
}  // namespace qtllm::infrastructure
