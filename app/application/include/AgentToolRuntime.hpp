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

    struct Dependencies
    {
        ValidateHandler validate;
        PolicyHandler policy;
        RiskHandler risk;
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

        [[nodiscard]] bool allowed() const
        {
            return errorMessage.isEmpty();
        }
    };

    explicit AgentToolRuntime(Dependencies dependencies = {});

    void setTools(QList<agent::ToolDefinition> tools);
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] std::optional<ToolDescriptor> inspect(
        const QString& qualifiedName) const;
    [[nodiscard]] agent::ToolValidationResult validate(
        const agent::Action& action) const;
    [[nodiscard]] infrastructure::mcp::ToolDecision policyDecision(
        const agent::Action& action) const;
    [[nodiscard]] QString callSignature(const agent::Action& action) const;
    [[nodiscard]] CallGuardResult guardCall(const agent::Action& action,
                                            bool isStatusPoll) const;
    void recordCallResult(const agent::Action& action,
                          agent::ToolOutcome outcome);
    void resetCallHistory();

   private:
    [[nodiscard]] ToolOperationKind operationKind(
        const agent::ToolDefinition& tool) const;

    Dependencies dependencies_;
    QList<agent::ToolDefinition> tools_;
    QString lastFailedCallSignature_;
    QStringList completedCallHistory_;
};
}  // namespace qtllm::application
