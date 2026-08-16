#pragma once

#include "AgentEvent.hpp"
#include "ChatMessage.hpp"

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

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
    int stagnationRecoveries = 0;
    int successfulToolResults = 0;
    bool completionReviewPerformed = false;
    QString pendingReviewedFinal;
    int lastPromptTokens = 0;
    int contextCompactions = 0;
    qsizetype lastSubmittedCharacters = 0;
    qsizetype requestMessageIndex = 0;
    QDateTime startedAt;
    QList<agent::Event> events;
    QList<chat::Message> inferenceMessages;
    QString toolRequestId;
};
}  // namespace qtllm::application

Q_DECLARE_METATYPE(qtllm::application::AgentRun::State)
