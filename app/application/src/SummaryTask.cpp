#include "SummaryTask.hpp"

#include "AgentPromptBuilder.hpp"

#include <utility>

namespace qtllm::application
{
SummaryTask::SummaryTask()
    : AgentTask(Kind::Summary, QStringLiteral("summary"),
                QStringLiteral("Summarize the completed work"))
{
}

void SummaryTask::activate(QList<chat::Message> messageSeed,
                           const QString& originalRequest,
                           const QJsonArray& completedTasks,
                           const QList<QJsonObject>& toolEvidence)
{
    messageSeed.append(AgentPromptBuilder::allPlanTasksCompletedMessage(
        originalRequest, completedTasks, toolEvidence));
    const auto requestMessageIndex = messageSeed.size() - 1;
    repairCount_ = 0;
    activateConversation(std::move(messageSeed), requestMessageIndex);
}

AgentTask::Directive SummaryTask::completeTaskGeneration(
    bool cancelled, const QList<QJsonObject>&, const QString&)
{
    const auto decision = completeDecision(cancelled);
    if (!decision.valid)
        return repair(decision.rawAction, decision.errorMessage);
    if (decision.action.type != agent::ActionType::Final)
        return repair(
            decision.rawAction,
            QStringLiteral(
                "All execution tasks are complete. The summary task accepts "
                "only one final action and cannot call tools or change task "
                "state."));

    messages().append(
        {chat::Role::Assistant, QString::fromUtf8(decision.rawAction)});
    complete();
    Directive result;
    result.type = Directive::Type::Completed;
    result.content = decision.action.content;
    return result;
}

AgentTask::Directive SummaryTask::repair(const QByteArray& rawAction,
                                         const QString& errorMessage)
{
    if (repairCount_ >= 1)
    {
        fail();
        Directive result;
        result.type = Directive::Type::Failed;
        result.code = QStringLiteral("summary_failed");
        result.detail = errorMessage;
        return result;
    }
    ++repairCount_;
    messages().append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    messages().append(
        AgentPromptBuilder::summaryCorrectionMessage(errorMessage));
    Directive result;
    result.type = Directive::Type::Generate;
    result.code = QStringLiteral("summary_repair");
    result.detail = errorMessage;
    return result;
}

QString SummaryTask::activity() const
{
    switch (status())
    {
        case Status::Pending:
            return QStringLiteral("Waiting to summarize the run");
        case Status::Running:
        case Status::WaitingForModel:
            return repairCount_ > 0
                       ? QStringLiteral("Repairing the final summary")
                       : QStringLiteral("Preparing the final answer");
        case Status::Completed:
            return QStringLiteral("Summary completed");
        case Status::Cancelled:
            return QStringLiteral("Summary stopped");
        case Status::Failed:
            return QStringLiteral("Summary failed");
        case Status::Blocked:
            return QStringLiteral("Summary blocked");
        case Status::WaitingForApproval:
        case Status::WaitingForTool:
            return QStringLiteral("Preparing the final answer");
    }
    return {};
}
}  // namespace qtllm::application
