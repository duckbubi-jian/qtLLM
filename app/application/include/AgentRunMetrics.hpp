#pragma once

#include "AgentRun.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace qtllm::application
{
struct AgentRunMetrics
{
    int schemaVersion = 1;
    QString runId;
    QString state;
    QString finishCode;
    QString finishMessage;
    QString failureCategory;
    QString firstToolChoice;
    int decisionCount = 0;
    int toolActionAttempts = 0;
    int toolValidationAttempts = 0;
    int toolValidationFailures = 0;
    int executedToolCalls = 0;
    int validationRepairs = 0;
    int duplicateToolActions = 0;
    int duplicateMutationActions = 0;
    int redundantDiscoveryCalls = 0;
    int pollRequests = 0;
    int contextCompactions = 0;
    int successfulToolResults = 0;
    int toolEvidenceCount = 0;
    double schemaValidArgumentRate = 1.0;
    QJsonArray taskTimings;

    [[nodiscard]] static AgentRunMetrics fromRun(const AgentRun& run);
    [[nodiscard]] QJsonObject toJson() const;
};
}  // namespace qtllm::application
