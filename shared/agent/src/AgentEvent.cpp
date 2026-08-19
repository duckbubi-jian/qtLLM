#include "AgentEvent.hpp"

namespace qtllm::agent
{
QString eventTypeName(EventType type)
{
    switch (type)
    {
        case EventType::RunStarted:
            return QStringLiteral("run_started");
        case EventType::TaskPlanAccepted:
            return QStringLiteral("task_plan_accepted");
        case EventType::TaskStarted:
            return QStringLiteral("task_started");
        case EventType::TaskStepUpdated:
            return QStringLiteral("task_step_updated");
        case EventType::DecisionStarted:
            return QStringLiteral("decision_started");
        case EventType::ApprovalRequested:
            return QStringLiteral("approval_requested");
        case EventType::ToolStarted:
            return QStringLiteral("tool_started");
        case EventType::ToolFinished:
            return QStringLiteral("tool_finished");
        case EventType::AnswerStarted:
            return QStringLiteral("answer_started");
        case EventType::RecoveryStarted:
            return QStringLiteral("recovery_started");
        case EventType::Warning:
            return QStringLiteral("warning");
        case EventType::Completed:
            return QStringLiteral("completed");
        case EventType::Blocked:
            return QStringLiteral("blocked");
        case EventType::Cancelled:
            return QStringLiteral("cancelled");
        case EventType::Failed:
            return QStringLiteral("failed");
    }
    return {};
}
}  // namespace qtllm::agent
