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
    ReviewToolCall,
    Blocked,
    Final
};

struct Action
{
    ActionType type = ActionType::Final;
    QString toolName;
    QJsonObject arguments;
    QString content;
    QJsonArray completionSteps;
    QString toolReviewVerdict;
    QString toolReviewDetail;
    QString blockReason;
    std::optional<bool> orderedPlan;
};

bool parseAction(const QByteArray& json, Action& action, QString& errorMessage);
QByteArray actionGrammar();
}  // namespace qtllm::agent
