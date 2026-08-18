#include "AgentRunMetrics.hpp"

#include "AgentEvent.hpp"

#include <algorithm>

namespace qtllm::application
{
namespace
{
QString stateName(AgentRun::State state)
{
    switch (state)
    {
        case AgentRun::State::Idle:
            return QStringLiteral("idle");
        case AgentRun::State::Deciding:
            return QStringLiteral("deciding");
        case AgentRun::State::WaitingForApproval:
            return QStringLiteral("waiting_for_approval");
        case AgentRun::State::ExecutingTool:
            return QStringLiteral("executing_tool");
        case AgentRun::State::GeneratingAnswer:
            return QStringLiteral("generating_answer");
        case AgentRun::State::Completed:
            return QStringLiteral("completed");
        case AgentRun::State::Cancelled:
            return QStringLiteral("cancelled");
        case AgentRun::State::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

QString failureCategory(const AgentRun& run)
{
    if (run.finishCode.isEmpty()) return {};
    const auto code = run.finishCode.toLower();
    if (code == QLatin1String("cancelled") ||
        code == QLatin1String("generation_cancelled") ||
        code == QLatin1String("tool_cancelled"))
        return QStringLiteral("cancelled");
    if (code.contains(QStringLiteral("transport")) ||
        code == QLatin1String("tool_side_effect_uncertain"))
        return QStringLiteral("transport_or_uncertain_side_effect");
    if (code.contains(QStringLiteral("protocol")))
        return QStringLiteral("protocol");
    if (code.contains(QStringLiteral("denied")))
        return QStringLiteral("authorization");
    if (code.contains(QStringLiteral("tool"))) return QStringLiteral("tool");
    if (code.contains(QStringLiteral("completion")) ||
        code.contains(QStringLiteral("verification")))
        return QStringLiteral("verification");
    if (code.contains(QStringLiteral("generation")))
        return QStringLiteral("generation");
    if (code.contains(QStringLiteral("invalid_agent")) ||
        code.contains(QStringLiteral("invalid_tool")) ||
        code.contains(QStringLiteral("task_plan")))
        return QStringLiteral("agent_action");
    return QStringLiteral("unknown");
}

QString firstToolChoice(const AgentRun& run)
{
    for (const auto& event : run.events)
        if (event.type == agent::EventType::ToolStarted) return event.toolName;
    return {};
}
}  // namespace

AgentRunMetrics AgentRunMetrics::fromRun(const AgentRun& run)
{
    AgentRunMetrics metrics;
    metrics.runId = run.id;
    metrics.state = stateName(run.state);
    metrics.finishCode = run.finishCode;
    metrics.finishMessage = run.finishMessage;
    metrics.failureCategory = failureCategory(run);
    metrics.firstToolChoice = firstToolChoice(run);
    metrics.decisionCount = run.decisionCount;
    metrics.toolActionAttempts = run.toolActionAttempts;
    metrics.toolValidationAttempts = run.toolValidationAttempts;
    metrics.toolValidationFailures = run.toolValidationFailures;
    metrics.executedToolCalls = run.executedToolCalls;
    metrics.validationRepairs = run.validationRepairs;
    metrics.duplicateToolActions = run.duplicateToolActions;
    metrics.duplicateMutationActions = run.duplicateMutationActions;
    metrics.redundantDiscoveryCalls = run.redundantDiscoveryCalls;
    metrics.pollRequests = run.pollRequests;
    metrics.completionReviewAttempts = run.completionReviewAttempts;
    metrics.completionReviewSuccesses = run.completionReviewSuccesses;
    metrics.contextCompactions = run.contextCompactions;
    metrics.successfulToolResults = run.successfulToolResults;
    metrics.toolEvidenceCount = run.evidenceRevision;
    metrics.schemaValidArgumentRate =
        run.toolValidationAttempts <= 0
            ? 1.0
            : std::clamp(static_cast<double>(run.toolValidationAttempts -
                                             run.toolValidationFailures) /
                             static_cast<double>(run.toolValidationAttempts),
                         0.0, 1.0);
    metrics.completionReviewSucceeded = run.completionReviewSuccesses > 0;
    return metrics;
}

QJsonObject AgentRunMetrics::toJson() const
{
    return {
        {QStringLiteral("schemaVersion"), schemaVersion},
        {QStringLiteral("runId"), runId},
        {QStringLiteral("state"), state},
        {QStringLiteral("finishCode"), finishCode},
        {QStringLiteral("finishMessage"), finishMessage},
        {QStringLiteral("failureCategory"), failureCategory},
        {QStringLiteral("firstToolChoice"), firstToolChoice},
        {QStringLiteral("decisionCount"), decisionCount},
        {QStringLiteral("toolActionAttempts"), toolActionAttempts},
        {QStringLiteral("toolValidationAttempts"), toolValidationAttempts},
        {QStringLiteral("toolValidationFailures"), toolValidationFailures},
        {QStringLiteral("executedToolCalls"), executedToolCalls},
        {QStringLiteral("validationRepairs"), validationRepairs},
        {QStringLiteral("schemaValidArgumentRate"), schemaValidArgumentRate},
        {QStringLiteral("duplicateToolActions"), duplicateToolActions},
        {QStringLiteral("duplicateMutationActions"), duplicateMutationActions},
        {QStringLiteral("redundantDiscoveryCalls"), redundantDiscoveryCalls},
        {QStringLiteral("pollRequests"), pollRequests},
        {QStringLiteral("completionReviewAttempts"), completionReviewAttempts},
        {QStringLiteral("completionReviewSuccesses"),
         completionReviewSuccesses},
        {QStringLiteral("completionReviewSucceeded"),
         completionReviewSucceeded},
        {QStringLiteral("contextCompactions"), contextCompactions},
        {QStringLiteral("successfulToolResults"), successfulToolResults},
        {QStringLiteral("toolEvidenceCount"), toolEvidenceCount}};
}
}  // namespace qtllm::application
