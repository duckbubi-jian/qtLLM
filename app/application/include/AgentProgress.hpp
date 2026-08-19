#pragma once

#include "AgentEvent.hpp"
#include "AgentRun.hpp"

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

namespace qtllm::application
{
enum class AgentProgressStepStatus
{
    Pending,
    Current,
    Completed,
    Blocked
};

struct AgentProgressStep
{
    QString id;
    QString description;
    AgentProgressStepStatus status = AgentProgressStepStatus::Pending;
    QString activity;
    qint64 elapsedMilliseconds = 0;
};

struct AgentProgressActivity
{
    agent::EventType type = agent::EventType::RunStarted;
    QString message;
    QString toolName;
    QDateTime timestamp;
};

struct AgentProgressSnapshot
{
    QString runId;
    AgentRun::State state = AgentRun::State::Idle;
    qint64 elapsedMilliseconds = 0;
    QList<AgentProgressStep> steps;
    QString currentStepId;
    QString operation;
    QString waitingReason;
    int completedToolCount = 0;
    int evidenceCount = 0;
    QList<AgentProgressActivity> recentActivity;
    QString finishCode;
    QString finishMessage;
};
}  // namespace qtllm::application

Q_DECLARE_METATYPE(qtllm::application::AgentProgressSnapshot)
