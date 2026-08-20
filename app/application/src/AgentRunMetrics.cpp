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
        case AgentRun::State::WaitingForVerification:
            return QStringLiteral("waiting_for_verification");
        case AgentRun::State::ExecutingTool:
            return QStringLiteral("executing_tool");
        case AgentRun::State::GeneratingAnswer:
            return QStringLiteral("generating_answer");
        case AgentRun::State::Completed:
            return QStringLiteral("completed");
        case AgentRun::State::Blocked:
            return QStringLiteral("blocked");
        case AgentRun::State::Cancelled:
            return QStringLiteral("cancelled");
        case AgentRun::State::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

QString failureCategoryForRun(const AgentRun& run)
{
    if (run.finishCode.isEmpty()) return {};
    const auto code = run.finishCode.toLower();
    if (code.startsWith(QStringLiteral("blocked_")))
        return QStringLiteral("blocked");
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
    if (code.contains(QStringLiteral("completion")) ||
        code.contains(QStringLiteral("verification")))
        return QStringLiteral("verification");
    if (code.contains(QStringLiteral("generation")))
        return QStringLiteral("generation");
    if (code.contains(QStringLiteral("invalid_agent")) ||
        code.contains(QStringLiteral("invalid_tool")) ||
        code.contains(QStringLiteral("task_plan")) ||
        code.contains(QStringLiteral("tool_call_review")) ||
        code == QLatin1String("agent_stalled") ||
        code == QLatin1String("decision_too_large"))
        return QStringLiteral("agent_action");
    if (code.contains(QStringLiteral("worker")) ||
        code.contains(QStringLiteral("inference")) ||
        code.contains(QStringLiteral("model")))
        return QStringLiteral("generation");
    if (code.contains(QStringLiteral("tool"))) return QStringLiteral("tool");
    return QStringLiteral("unknown");
}

QString firstToolChoiceForRun(const AgentRun& run)
{
    for (const auto& event : run.events)
        if (event.type == agent::EventType::ToolStarted) return event.toolName;
    return {};
}

QString taskStatusName(AgentTask::Status status)
{
    switch (status)
    {
        case AgentTask::Status::Pending:
            return QStringLiteral("pending");
        case AgentTask::Status::Running:
            return QStringLiteral("running");
        case AgentTask::Status::WaitingForModel:
            return QStringLiteral("waiting_for_model");
        case AgentTask::Status::WaitingForApproval:
            return QStringLiteral("waiting_for_approval");
        case AgentTask::Status::WaitingForTool:
            return QStringLiteral("waiting_for_tool");
        case AgentTask::Status::Completed:
            return QStringLiteral("completed");
        case AgentTask::Status::Blocked:
            return QStringLiteral("blocked");
        case AgentTask::Status::Cancelled:
            return QStringLiteral("cancelled");
        case AgentTask::Status::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

QJsonObject taskTiming(const AgentTask& task)
{
    const auto snapshot = task.runtimeSnapshot();
    return {
        {QStringLiteral("id"), snapshot.id},
        {QStringLiteral("description"), snapshot.description},
        {QStringLiteral("kind"), AgentTask::kindName(snapshot.kind)},
        {QStringLiteral("status"), taskStatusName(snapshot.status)},
        {QStringLiteral("elapsedMilliseconds"), snapshot.elapsedMilliseconds}};
}
}  // namespace

AgentRunMetrics AgentRunMetrics::fromRun(const AgentRun& run)
{
    AgentRunMetrics metrics;
    metrics.runId = run.id;
    metrics.state = stateName(run.state);
    metrics.finishCode = run.finishCode;
    metrics.finishMessage = run.finishMessage;
    metrics.failureCategory = failureCategoryForRun(run);
    metrics.firstToolChoice = firstToolChoiceForRun(run);
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
    if (run.planningTask.has_value())
        metrics.taskTimings.append(taskTiming(*run.planningTask));
    if (run.directTask.has_value())
        metrics.taskTimings.append(taskTiming(*run.directTask));
    for (const auto& task : run.executionTasks)
        metrics.taskTimings.append(taskTiming(task));
    if (run.summaryTask.has_value())
        metrics.taskTimings.append(taskTiming(*run.summaryTask));
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
        {QStringLiteral("contextCompactions"), contextCompactions},
        {QStringLiteral("successfulToolResults"), successfulToolResults},
        {QStringLiteral("toolEvidenceCount"), toolEvidenceCount},
        {QStringLiteral("taskTimings"), taskTimings}};
}
}  // namespace qtllm::application
