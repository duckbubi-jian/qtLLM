#pragma once

#include "AgentEvent.hpp"
#include "ChatMessage.hpp"

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

namespace qtllm::application
{
struct AgentRun
{
    enum class State
    {
        Idle,
        Deciding,
        WaitingForApproval,
        ExecutingTool,
        GeneratingAnswer,
        Completed,
        Cancelled,
        Failed
    };

    QString id;
    QString userRequest;
    State state = State::Idle;
    int repairAttempts = 0;
    QDateTime startedAt;
    QList<agent::Event> events;
    QList<chat::Message> inferenceMessages;
    QString toolRequestId;
};
}  // namespace qtllm::application

Q_DECLARE_METATYPE(qtllm::application::AgentRun::State)
