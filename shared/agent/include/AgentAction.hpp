#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace qtllm::agent
{
enum class ActionType
{
    CallTool,
    TaskPlan,
    ReviewCompletion,
    Final
};

struct Action
{
    ActionType type = ActionType::Final;
    QString toolName;
    QJsonObject arguments;
    QString content;
    QString completionVerdict;
    QJsonArray completionSteps;
    QString completionDetail;
};

bool parseAction(const QByteArray& json, Action& action, QString& errorMessage);
QByteArray actionGrammar();
}  // namespace qtllm::agent
