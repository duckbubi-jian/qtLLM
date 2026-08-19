#pragma once

#include "AgentAction.hpp"
#include "ToolDefinition.hpp"
#include "ToolOperationKind.hpp"
#include "ToolPolicy.hpp"
#include "ToolResult.hpp"
#include "ToolValidation.hpp"

#include <QList>
#include <QStringList>

#include <functional>
#include <optional>

namespace qtllm::application
{
class AgentToolRuntime final
{
   public:
    using ValidateHandler = std::function<agent::ToolValidationResult(
        const QString&, const QJsonObject&)>;
    using PolicyHandler =
        std::function<infrastructure::mcp::ToolDecision(const QString&)>;
    using RiskHandler =
        std::function<infrastructure::mcp::ToolRisk(const QString&)>;
    using CallHandler =
        std::function<QString(const QString&, const QJsonObject&)>;
    using CancelHandler = std::function<void(const QString&)>;

    struct Dependencies
    {
        ValidateHandler validate;
        PolicyHandler policy;
        RiskHandler risk;
        CallHandler call;
        CancelHandler cancel;
    };

    struct ToolDescriptor
    {
        agent::ToolDefinition definition;
        ToolOperationKind operationKind = ToolOperationKind::Unknown;
    };

    struct CallGuardResult
    {
        QString signature;
        QString errorMessage;
        bool statusPoll = false;

        [[nodiscard]] bool allowed() const
        {
            return errorMessage.isEmpty();
        }
    };

    struct DispatchResult
    {
        enum class Status
        {
            Started,
            Delayed,
            Failed
        };

        Status status = Status::Failed;
        int delayMilliseconds = 0;
        bool statusPoll = false;
    };

    struct CompletedCall
    {
        agent::Action action;
        agent::ToolResult result;
        ToolOperationKind operationKind = ToolOperationKind::Unknown;
    };

    explicit AgentToolRuntime(Dependencies dependencies = {});

    void setTools(QList<agent::ToolDefinition> tools);
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] std::optional<ToolDescriptor> inspect(
        const QString& qualifiedName) const;
    [[nodiscard]] agent::ToolValidationResult validate(
        const agent::Action& action) const;
    [[nodiscard]] infrastructure::mcp::ToolDecision authorize(
        const agent::Action& action);
    [[nodiscard]] const std::optional<agent::Action>& pendingApproval() const;
    [[nodiscard]] std::optional<agent::Action> resolveApproval(bool approved);
    void clearPendingApproval();
    [[nodiscard]] QString callSignature(const agent::Action& action) const;
    [[nodiscard]] CallGuardResult guardCall(const agent::Action& action,
                                            bool isStatusPoll = false) const;
    void recordCallResult(const agent::Action& action,
                          agent::ToolOutcome outcome);
    void resetCallHistory();
    [[nodiscard]] DispatchResult dispatch(const agent::Action& action,
                                          qint64 nowMilliseconds);
    [[nodiscard]] std::optional<CompletedCall> completeCall(
        const agent::ToolResult& result, qint64 nowMilliseconds);
    [[nodiscard]] std::optional<agent::Action> takePendingPoll();
    [[nodiscard]] const std::optional<agent::Action>& activeAction() const;
    [[nodiscard]] const std::optional<agent::Action>& pendingPoll() const;
    void cancelActiveCall();
    void clearTransportState();

   private:
    [[nodiscard]] ToolOperationKind operationKind(
        const agent::ToolDefinition& tool) const;

    Dependencies dependencies_;
    QList<agent::ToolDefinition> tools_;
    std::optional<agent::Action> pendingApproval_;
    std::optional<agent::Action> activeAction_;
    std::optional<agent::Action> pendingPoll_;
    QString activeCallSignature_;
    QString activeRequestId_;
    QString pollableCallSignature_;
    qint64 lastPollCompletedAtMs_ = 0;
    QString lastFailedCallSignature_;
    QStringList completedCallHistory_;
};
}  // namespace qtllm::application
