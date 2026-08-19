#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace qtllm::agent
{
enum class ActionType
{
    CallTool,
    TaskPlan,
    ReviewPlanStep,
    ReviewCompletion,
    Blocked,
    Final
};

struct Action
{
    ActionType type = ActionType::Final;
    QString toolName;
    QString planStepId;
    std::optional<bool> completesPlanStep;
    QJsonObject arguments;
    QString content;
    QString completionVerdict;
    QJsonArray completionSteps;
    QString completionDetail;
    QString planStepReviewStatus;
    QJsonArray planStepReviewEvidence;
    QString blockReason;
    std::optional<bool> orderedPlan;
};

bool parseAction(const QByteArray& json, Action& action, QString& errorMessage);
QByteArray actionGrammar();
}  // namespace qtllm::agent
