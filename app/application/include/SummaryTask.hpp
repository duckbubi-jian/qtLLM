#pragma once

#include "AgentTask.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>

namespace qtllm::application
{
class SummaryTask final : public AgentTask
{
   public:
    SummaryTask();

    void activate(QList<chat::Message> messageSeed,
                  const QString& originalRequest,
                  const QJsonArray& completedTasks,
                  const QList<QJsonObject>& toolEvidence);
    [[nodiscard]] Directive completeTaskGeneration(
        bool cancelled, const QList<QJsonObject>& toolEvidence,
        const QString& unresolvedVerificationReason) override;

   private:
    [[nodiscard]] Directive repair(const QByteArray& rawAction,
                                   const QString& errorMessage);
    [[nodiscard]] QString activity() const override;

    int repairCount_ = 0;
};
}  // namespace qtllm::application
