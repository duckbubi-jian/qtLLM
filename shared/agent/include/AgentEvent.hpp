#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace qtllm::agent
{
enum class EventType
{
    RunStarted,
    DecisionStarted,
    ApprovalRequested,
    ToolStarted,
    ToolFinished,
    AnswerStarted,
    Completed,
    Cancelled,
    Failed
};

struct Event
{
    QString runId;
    EventType type = EventType::RunStarted;
    QString message;
    QString toolName;
    QJsonObject data;
    QDateTime timestamp = QDateTime::currentDateTimeUtc();
};

QString eventTypeName(EventType type);
}  // namespace qtllm::agent

Q_DECLARE_METATYPE(qtllm::agent::Event)
