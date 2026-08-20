#pragma once

#include "AgentAction.hpp"
#include "AssistantContext.hpp"
#include "ChatMessage.hpp"
#include "ToolDefinition.hpp"
#include "ToolResult.hpp"
#include "ToolValidation.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace qtllm::application
{
class AgentPromptBuilder final
{
   public:
    static QList<chat::Message> initialMessages(
        const QString& userRequest, const QList<agent::ToolDefinition>& tools,
        const QList<chat::Message>& conversationHistory = {},
        const AssistantContext& context = {});
    [[nodiscard]] static bool requiresCompletionReview(
        const QString& userRequest);
    static chat::Message taskPlanMessage(const QString& originalRequest);
    static chat::Message planTaskActivationMessage(
        const QJsonObject& currentStep, const QJsonArray& completedSteps,
        const QList<QJsonObject>& priorToolEvidence);
    static chat::Message allPlanTasksCompletedMessage(
        const QString& originalRequest, const QJsonArray& steps,
        const QList<QJsonObject>& toolEvidence);
    static chat::Message summaryCorrectionMessage(const QString& errorMessage);
    static chat::Message taskPlanCorrectionMessage(const QString& errorMessage);
    static chat::Message toolResultMessage(const agent::ToolResult& result,
                                           const QJsonObject& ledgerState,
                                           const QString& verificationReason,
                                           const QString& recoveryGuidance);
    static chat::Message unfinishedFinalMessage(const QString& errorMessage);
    static chat::Message correctionMessage(const QString& errorMessage);
    static chat::Message toolValidationCorrectionMessage(
        const agent::ToolDefinition& tool, const QJsonObject& arguments,
        const agent::ToolValidationIssue& issue);
    static chat::Message noProgressMessage(const QString& errorMessage);
    static chat::Message orderedPlanCorrectionMessage(
        const QString& errorMessage);
};
}  // namespace qtllm::application
