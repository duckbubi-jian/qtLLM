#pragma once

#include <QString>

namespace qtllm::infrastructure
{
class SettingsStore final
{
   public:
    explicit SettingsStore(QString filePath = {});

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QString lastModelPath() const;
    bool setLastModelPath(const QString& modelPath) const;

   private:
    QString filePath_;
};
}  // namespace qtllm::infrastructure
