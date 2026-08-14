#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace qtllm::agent
{
enum class ActionType
{
    CallTool,
    Final
};

struct Action
{
    ActionType type = ActionType::Final;
    QString toolName;
    QJsonObject arguments;
    QString content;
};

bool parseAction(const QByteArray& json, Action& action, QString& errorMessage);
QByteArray actionGrammar();
}  // namespace qtllm::agent
