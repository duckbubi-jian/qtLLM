#include "PlanningTask.hpp"

#include "AgentPromptBuilder.hpp"

#include <QRegularExpression>

#include <utility>

namespace qtllm::application
{
namespace
{
int numberedInstructionCount(const QString& request)
{
    static const QRegularExpression numberedInstruction(
        QStringLiteral(R"(^\s*\d+\s*[.):]\s+)"));
    auto count = 0;
    for (const auto& line : request.split(QLatin1Char('\n')))
        if (numberedInstruction.match(line).hasMatch()) ++count;
    return count;
}
}  // namespace

PlanningTask::PlanningTask(QString originalRequest, QStringList availableTools)
    : AgentTask(Kind::Planning, QStringLiteral("planning"),
                QStringLiteral("Plan the requested work")),
      availableTools_(),
      numberedInstructionCount_(numberedInstructionCount(originalRequest))
{
    for (const auto& tool : availableTools)
        availableTools_.insert(tool);
}

void PlanningTask::activate(QList<chat::Message> messages)
{
    repairCount_ = 0;
    const auto requestMessageIndex = messages.size() - 1;
    activateConversation(std::move(messages), requestMessageIndex);
}

AgentTask::Directive PlanningTask::completeTaskGeneration(
    bool cancelled, const QList<QJsonObject>&, const QString&)
{
    const auto decision = completeDecision(cancelled);
    if (!decision.valid)
        return repair(decision.rawAction, decision.errorMessage);
    if (decision.action.type != agent::ActionType::TaskPlan)
        return repair(
            decision.rawAction,
            QStringLiteral(
                "A task_plan is required before executing or completing "
                "this multi-step request."));

    const auto errorMessage = validatePlan(decision.action);
    if (!errorMessage.isEmpty())
        return repair(decision.rawAction, errorMessage);

    messages().append(
        {chat::Role::Assistant, QString::fromUtf8(decision.rawAction)});
    complete();
    Directive result;
    result.type = Directive::Type::TasksCreated;
    result.tasks = decision.action.completionSteps;
    return result;
}

int PlanningTask::repairCount() const
{
    return repairCount_;
}

QString PlanningTask::validatePlan(const agent::Action& action) const
{
    if (!action.orderedPlan.value_or(false))
        return QStringLiteral(
            "task_plan must explicitly include ordered=true so the "
            "scheduler can enforce step-by-step execution.");
    if (numberedInstructionCount_ >= 2 &&
        action.completionSteps.size() > numberedInstructionCount_)
        return QStringLiteral(
                   "The plan has %1 tasks for %2 numbered user "
                   "instructions. Use at most one task per numbered "
                   "instruction. Keep dependent focus, lookup, inspection, "
                   "edit, import, parse, and verification calls inside the "
                   "same outcome task instead of creating tool-level tasks.")
            .arg(action.completionSteps.size())
            .arg(numberedInstructionCount_);

    auto sawNonToolStep = false;
    for (const auto& value : action.completionSteps)
    {
        const auto step = value.toObject();
        const auto requiresTool =
            step.value(QStringLiteral("requires_tool")).toBool();
        for (const auto& allowedValue :
             step.value(QStringLiteral("allowed_tools")).toArray())
        {
            const auto allowedTool = allowedValue.toString();
            if (!availableTools_.contains(allowedTool))
                return QStringLiteral(
                           "Task-plan step '%1' assigns unavailable tool "
                           "'%2'. Use only exact qualified names from the "
                           "available tool catalog.")
                    .arg(step.value(QStringLiteral("id")).toString(),
                         allowedTool);
        }
        if (!requiresTool)
            sawNonToolStep = true;
        else if (sawNonToolStep)
            return QStringLiteral(
                "An ordered task plan cannot place a tool-required step "
                "after a non-tool step. Keep all external operations in "
                "user order and reserve non-tool steps for the trailing "
                "final response.");
    }
    return {};
}

AgentTask::Directive PlanningTask::repair(const QByteArray& rawAction,
                                          const QString& errorMessage)
{
    if (repairCount_ >= 1)
    {
        fail();
        Directive result;
        result.type = Directive::Type::Failed;
        result.code = QStringLiteral("task_plan_failed");
        result.detail = errorMessage;
        return result;
    }
    ++repairCount_;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    messages().append(
        AgentPromptBuilder::taskPlanCorrectionMessage(errorMessage));
    Directive result;
    result.type = Directive::Type::Generate;
    result.code = QStringLiteral("task_plan_repair");
    result.detail = errorMessage;
    return result;
}

QString PlanningTask::activity() const
{
    switch (status())
    {
        case Status::Pending:
            return QStringLiteral("Waiting to plan the request");
        case Status::Running:
        case Status::WaitingForModel:
            return repairCount_ > 0 ? QStringLiteral("Repairing the task plan")
                                    : QStringLiteral("Planning the request");
        case Status::Completed:
            return QStringLiteral("Plan accepted");
        case Status::Cancelled:
            return QStringLiteral("Planning stopped");
        case Status::Failed:
            return QStringLiteral("Planning failed");
        case Status::Blocked:
            return QStringLiteral("Planning blocked");
        case Status::WaitingForApproval:
        case Status::WaitingForTool:
            return QStringLiteral("Planning the request");
    }
    return {};
}
}  // namespace qtllm::application
