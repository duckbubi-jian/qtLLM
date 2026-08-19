#pragma once

#include "AgentEvent.hpp"
#include "AgentLedger.hpp"
#include "ChatMessage.hpp"

#include <QDateTime>
#include <QJsonArray>
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
    int validationRepairs = 0;
    int consecutiveValidationFailures = 0;
    int stagnationRecoveries = 0;
    int successfulToolResults = 0;
    int readOnlyTransportRetries = 0;
    int decisionCount = 0;
    int toolActionAttempts = 0;
    int toolValidationAttempts = 0;
    int toolValidationFailures = 0;
    int executedToolCalls = 0;
    int duplicateToolActions = 0;
    int duplicateMutationActions = 0;
    int redundantDiscoveryCalls = 0;
    int consecutiveDiscoveryCalls = 0;
    int pollRequests = 0;
    int completionReviewAttempts = 0;
    int completionReviewSuccesses = 0;
    int evidenceRevision = 0;
    int lastReviewedEvidenceRevision = -1;
    int completionReviewsAtRevision = 0;
    int taskPlanFailures = 0;
    int orderedPlanRepairs = 0;
    int completionReviewFailures = 0;
    int completionPlanDriftRepairs = 0;
    bool taskPlanRequired = false;
    bool orderedTaskPlan = false;
    bool awaitingCompletionReview = false;
    int currentPlanStepEvidenceStart = 1;
    QJsonArray completionSteps;
    QString pendingFinalCandidate;
    int lastPromptTokens = 0;
    int contextCompactions = 0;
    qsizetype lastSubmittedCharacters = 0;
    qsizetype requestMessageIndex = 0;
    QDateTime startedAt;
    QList<agent::Event> events;
    QList<chat::Message> inferenceMessages;
    QString toolRequestId;
    QString finishCode;
    QString finishMessage;
    AgentLedger ledger;
};
}  // namespace qtllm::application

Q_DECLARE_METATYPE(qtllm::application::AgentRun::State)
