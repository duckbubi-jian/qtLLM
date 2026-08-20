#pragma once

#include "AgentTask.hpp"

#include <QJsonArray>
#include <QSet>
#include <QStringList>

namespace qtllm::application
{
class PlanningTask final : public AgentTask
{
   public:
    explicit PlanningTask(QString originalRequest, QStringList availableTools);

    void activate(QList<chat::Message> messages);
    [[nodiscard]] Directive completeTaskGeneration(
        bool cancelled, const QList<QJsonObject>& toolEvidence,
        const QString& unresolvedVerificationReason) override;
    [[nodiscard]] int repairCount() const;

   private:
    [[nodiscard]] QString validatePlan(const agent::Action& action) const;
    [[nodiscard]] Directive repair(const QByteArray& rawAction,
                                   const QString& errorMessage);
    [[nodiscard]] QString activity() const override;

    QSet<QString> availableTools_;
    int numberedInstructionCount_ = 0;
    int repairCount_ = 0;
};
}  // namespace qtllm::application
